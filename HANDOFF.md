# httpp hand-off (consolidated)

This one file replaces handoff.md and handoff_5 to handoff_14. Read it first; the ADRs hold the reasoning. Older files can be deleted or kept for history.
Owner: Mohammad (GitHub `mohammadraziei`), the author of httpp. Date of this file: 2026-10-30.

## 1. What httpp is
A C++ HTTP library (async server on Asio + llhttp, clients, downloads, progress bars, URL, WebSocket) with a Cython/Python layer. Direction (ADR-0001):
httpp replaces FastAPI/Starlette above and uvicorn below: a Starlette/FastAPI-like API implemented natively on our own engine, launched by `httpp run main:app` or `app.run()`.
Handlers run on a standard asyncio loop (ADR-0002); no ASGI adapter is needed (low priority). One design for the C++ and the Python API; only GIL related differences.

## 2. Rules from the owner (standing)
- **Talk to him in Persian.** Everything inside the repo (docs, ADRs, comments, tests, changelog) is English with **no em dash**.
- **Never push.** He commits and pushes himself. After every step build the whole project as `httpp.zip` (`cd <parent> && zip -qr httpp.zip httpp -x '*/__pycache__/*' '*/build/*' '*/.git/*' '*/.venvs/*' '*/.build/*' '*/bin/*'`) and send it with SendUserFile, without asking.
- He delegates technical decisions ("do the most principled, optimal thing, the ADRs may be wrong"): decide, say what and why, record it in an ADR. The old golden rule (ask before critical architecture decisions, `docs/agent.md` section 1) was explicitly relaxed by him (ADR-0022); still show the reasoning.
- Be honest: say what was measured and what was not, own earlier mistakes (he catches overclaiming). Numbers are comparable only inside one interleaved run.
- He answers "continue" / "you decide" when he does not want to choose: then decide and record. He asks "I don't understand" for terse tables: explain in plain words.
- TDD (tests before code). "ponytail" style: simplest working solution, a `ponytail:` comment on every deliberate simplification with its upgrade path; full validation, error handling and security still apply. Correctness work is not skipped for tricks.
- **Zero system dependencies** (nothing from apt/brew; vendor or submodule; pip build deps are fine). STL-like snake_case in C++.
- `cpp-httplib` (`src/third_party/cpp-httplib/httplib.h`, v0.54.1) stays byte-identical to upstream; llhttp unmodified; document any vendor change in `src/third_party/README.md`. Asio, mbedtls, liburlparser are submodules.
- Be honest about "async" and the GIL: say what is truly non-blocking and what is a thread pool.
- macOS wheels target 10.13: avoid `std::filesystem`, floating `to_chars`, `std::optional` accessors, other post-10.13 libc++ APIs in `src/core`.
- Permission note: the sandbox's safety classifier once refused a workflow that writes to a GitHub release until he confirmed; do not work around a refusal, explain and ask.

## 3. Architecture today
```
include/httpp/        public API (client, server, websocket, download, url, progress, router ...)
src/core/             async_server.cpp (Asio engine + llhttp; tunnel for upgrades), server.cpp (router, hooks, static files, run_ws),
                      websocket.cpp + internal/websocket.hpp (RFC 6455 session), websocket_api.cpp, async_http.cpp (async client),
                      client.cpp / download.cpp / curl.cpp (httplib, sync), inflate/deflate/content_decode (vendored stb codecs), log.cpp, tls_session.cpp
src/bindings/python/  httpp_cy.pyx (Cython glue, inbox, lanes, _WsConn), httpp/server.py (Server, adapters, lanes, _adapt_ws),
                      _params.py (plans: Query/Path/Header/Cookie/Body/Depends), _websocket.py, _run.py, _validate*.py, _router.py, ...
tests/cpp  tests/python  benchmarks/  tools/autobahn/  docs/adr/  docs/Architecture.md  checklist.md
```
Request paths:
- **`def` route**: I/O thread (parse + match) -> C++ worker pool thread -> takes the GIL -> handler. Pool starts at 1 thread with a GIL and grows after a 10 ms wait (ADR-0025).
- **`async def` route**: I/O thread -> `route_io` -> per-lane inbox (mutex + deque, wake byte to a socket; no GIL on the I/O thread) -> asyncio loop thread of that lane drains a burst with the GIL held once -> task -> `done()`.
  A handler that cannot suspend may run inline on the I/O thread when its loop sleeps (gate; ADR-0023); default on only with 2 CPUs or fewer.
- **Lanes (ADR-0024)**: `set_io_threads(n)` = one engine per I/O thread on one port (SO_REUSEPORT). Python: one asyncio loop per I/O thread only on a free-threaded build, else one loop. Default `io_threads` = `min(4, CPUs // 4)` with a GIL, 1 without a GIL or on Windows.
- `after_request`/`error_handler` hooks make `done` go through `run_in_executor` (they run inside `done()`; an `async def` hook would deadlock the loop).
- **WebSocket C++ (ADR-0026)**: generic engine "tunnel" (`wire_response::upgrade`, `tunnel_io`); `ws_session` implements frames, fragments, control frames, close handshake (5 s), UTF-8 and close-code checks, `max_message` at the frame header, bounded send queue (`status::full`, `on_drain` at half, `pause_reading`/`resume_reading`), ping keep-alive. PSA SHA-1 for the accept key. No permessage-deflate, no TLS of its own.
- **WebSocket handshake (ADR-0027, amends 0026)**: `ws_handler::on_handshake(shared_ptr<const request>, ws_acceptor)`; accept/reject from any thread, first call wins, dropped = connection closed, unanswered = 504. `websocket::user()` (shared_ptr<void> given at accept). `on_close` exactly once for every accepted upgrade (1006 if it never opened). The engine closes with a lingering close (half-close, waits up to 2 s for the peer's FIN), so the final close code can arrive after the close frames.
- **WebSocket Python (ADR-0027)**: `@app.websocket(path, origins=None, max_message, max_buffered, max_queued, ping_interval)` with `async def h(websocket)`; Starlette-shaped (`accept`, `receive*`, `async for`, `send_*`, `ping`, `close`, `WebSocketDisconnect`). Per-connection C++ mailbox (`httpp_py_wsconn`) filled on the I/O thread without the GIL, one wake per burst via the lane inbox. Receive is bounded by `max_queued` (socket not read past it, resumes at half), `await send_*` waits for `on_drain` when the queue is full. Default Origin policy: same origin only (Origin host == Host), `origins=["*"]` allows all. Not supported on WebSocket routes: dependencies, `response`, background tasks, OpenAPI, `APIRouter.websocket`.

## 4. State (2026-10-30)
- C++: 374 tests pass (43 session + 35 server WebSocket). TSan: clean on the WebSocket tests **before** the async-handshake rework; not re-run after it; the rest of the suite was never run under TSan. ASan only on the engine earlier.
- Python: 652 passed, 4 skipped (3.14 GIL), 651 passed, 5 skipped (3.14t); 27 are WebSocket tests (`tests/python/test_websocket.py`). 3.13 not re-run after WebSocket.
- ADRs 0001 to 0030 are in `docs/adr/` (index in its README). Recent: 0019 vendored deflate/inflate (gzip/deflate in the clients), 0020 HTTPException/response classes, 0021 pool/two-core numbers, 0022 dual API rule, 0023 inline async, 0024 lanes, 0025 measured defaults + the crash, 0026/0027 WebSocket.
- Feature level: FastAPI-like layer (Query/Path/Header/Cookie/Body/Depends, forms/uploads, OpenAPI + Swagger UI compiled in, validators in Cython, response classes, HTTPException, BackgroundTasks, APIRouter), `run(workers=N)` (Linux), logging bridge.
- Autobahn: `tools/autobahn/` (echo server, config, report summariser) and `.github/workflows/autobahn.yml` (runs on push to master and by hand, uploads `autobahn-report.md` to the `__beta__` release and the full report as an artifact). **First run done (2026-10-31): no FAILED, 585 OK, 11 NON-STRICT, 6 INFORMATIONAL.** The workflow is now a job of the orchestrator (ADR-0028). The only real finding, UTF-8 checked at message end (6.4.x), is fixed in ADR-0029 (next run must show it OK). The other NON-STRICT ones (3.2, 3.3, 4.1.3, 4.1.4, 4.2.3, 4.2.4, 5.15) are allowed by the RFC. The docker daemon does not exist in the sandbox and `autobahntestsuite` from PyPI is Python 2 code (str vs bytes payloads make a Python 3 run untrustworthy), so the owner runs it through CI and sends the summary. The orchestrator workflow deletes `__beta__` assets at each master push.

## 5. Build and test
- Sandbox: 2 cores. Venvs via `uv`: `uv venv --python 3.14 venv314`, `uv venv --python 3.14t venv314t`; install build deps (`cmake ninja cython jsonschema jinja2 cython-cmake pytest pytest-timeout scikit-build-core pydantic uvicorn uvloop httptools fastapi`); install the project with `VIRTUAL_ENV=<venv> uv pip install --python <venv>/bin/python ./httpp` (about 100 s; a `.pyx` change needs this, a pure-Python change can be copied over `site-packages/httpp/`). `UV_NATIVE_TLS` warning is harmless. Run Python tests with `<venv>/bin/python -m pytest httpp/tests/python`.
- C++ tests: `cmake -S . -B ../build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHTTPP_BUILD_PYTHON=OFF -DHTTPP_BUILD_TESTS=ON`; build only `--target test_httpp_cpp` (the default target fails in the nested Python build); run `tests/cpp/test_httpp_cpp --filter='httpp_ws*'` (glob, not a prefix). The test target compiles `websocket.cpp` and `async_server.cpp` in itself because internals are not exported.
- TSan recipe: separate build dir with `-DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" -DCMAKE_C_FLAGS=... -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread -DHTTPP_BUILD_PYTHON=OFF`. Do not configure two build dirs in parallel (the liburlparser configure rewrites a tracked `public_suffix_list.dat` and needs the network; it is the owner's library, mention it if it comes up).
- Long shell loops can exceed the 10 minute tool limit: run them in the background and poll.
- Benchmarks are a CMake project (ADR-0030): `cmake -S benchmarks -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release`, then `cmake --build build-bench --target bench_quick` (about 5 min) or `--target bench` (about 25 min); settings are cache variables (`-DBENCH_SECONDS=...`, `-DBENCH_ONLY=ws,idle`, list in `benchmarks/CMakeLists.txt` and `benchmarks/README.md`). It fetches Drogon and jsoncpp (needs the libuuid headers) and builds `ws_load`, the WebSocket load generator. The Makefile is gone. Owner's machine: Ubuntu 26.04, i7-13620H, 16 threads (P-cores 0-11, E-cores 12-15), project on a Windows drive under /mnt/windowsD; core-aligned split (server 0-7, load generator 8-15); wants the `performance` governor.
- Profiling that worked: `py-spy record --gil`, `strace -c`, `/proc/<pid>/task/*/stat`. `perf` is not installed. Outbound traffic is an allowlist proxy (PyPI, crates.io, GitHub work; docker daemon absent).

## 6. Reference numbers (owner's machine, core-aligned split, 64 connections, req/s, medians of 3; the old shared-hyper-thread split numbers are void)
- hello, py 3.14 GIL: `def` 211k / 348k / 502k / 423k for 1 / 2 / 4 / 8 I/O threads; `async def` 193k / 313k / 300k / 270k; uvicorn 83k; FastAPI 22k.
- hello, py 3.14t: `def` 211k / 354k / 516k / 407k; `async def` 195k / 315k / 477k / 443k; inline 145k / 267k / 351k / 529k (not monotone, unexplained). C++ `route` 206k / 376k / 513k / 695k, `route_io` 203k / 377k / 527k / 792k.
- CPU-bound (about 60 us): GIL 24-25k at every size (only p99 differs); 3.14t `def` 65-70k, `async def` with lanes 23k / 41k / 67k / 73k; uvicorn 20k. JSON on 3.14t: `def` plateaus near 300k, `async def` 434k at 4.
- One connection: inline 60-66k, `def` 44-53k, `async def` 40-43k, C++ `route_io` 74k. Fixed rate at half of max: p99.9 0.8-1.3 ms.
- WebSocket: **no benchmark exists.**

## 6b. Bench run 2026-10-31 (tree be18e38c71, governor powersave, NOT performance; 3 rounds, 64 connections)
- **0 errors in all ~100 configurations** (about 300 rounds). The old loss rate (2 of about 150 rounds) predicts about 4: this is weak evidence that the `http_date` fix was the cause, not proof.
- hello py 3.14 GIL `def` 213k / 347k / 495k / 412k; `async def` 199k / 316k / 311k / 279k; 3.14t `def` 210k / 367k / 486k / 408k, `async def` 203k / 295k / 475k / 451k; C++ `route_io` 222k / 392k / 533k / 808k. Within about 6 percent of section 6 (run-to-run noise); a cross-run comparison cannot settle the WebSocket inbox question, that needs an interleaved A/B.
- 3.14t CPU-bound `def`, io=1: pool=2 gives 76.4k against 64.9k for the default (+18 percent, the same as before); at io=4 only +2 percent. First-burst cost still unmeasured. Free-threaded default is io_threads=1, so the default setup would gain.
- 3.14t `def JSON`: 329k at 2 I/O threads, then 287k and 278k (plateau, unexplained, needs `perf`). `async def JSON` 440k at 4.
- One connection: inline 71k, `def` 57k, `async def` 47k, C++ `route_io` 84k (higher than section 6, different machine state).

## 7. Open problems and next steps (in order)
1. **Autobahn**: done once (see section 4). After the next push check that 6.4.x are OK. Cases 12/13 (permessage-deflate) are excluded by design.
2. **Crash from the earlier bench** (one clean run on 2026-10-31, see 6b; keep watching): two bench rounds lost the server (py `def` GIL io=8 and C++ `route` io=4). A data race in `http_date()` was found by TSan and fixed (thread_local); not proven to be the cause. The next `make bench` decides (error rows gone or not). If one persists: core dump / faulthandler, TSan the Python extension. Also grep `src/core` and the bindings for function-level statics and shared mutable state that only fail with several loops.
3. **Re-run the `bench` target** (governor `performance`): it must show no error rows and no HTTP regression from the WebSocket inbox change (the inbox item grew by a `shared_ptr` and a flag, `_drain_inbox` has one extra check; not measured). Open readings: 3.14t `def` pool should start at 2? (+17 percent at 1 I/O thread, slower first burst; `grow_after` 2 ms untested), `def JSON` plateau on 3.14t (needs `perf`: suspects refcount contention, pool queue lock), non-monotone inline on 3.14t.
4. **WebSocket benchmark and idle-connection memory: written (ADR-0030, `BENCH_ONLY=ws,idle`), not yet run on the owner's machine.** In a 2-core sandbox smoke test, one idle connection cost about 20 KiB in httpp (C++ and Python) against about 9 KiB in Drogon: worth a look (suspects: per-connection buffers). TSan after the async-handshake rework (WebSocket tests, then the whole suite).
5. WebSocket gaps: `APIRouter.websocket`, dependencies, OpenAPI note, `on_close` earlier than the lingering close (considered, kept), permessage-deflate (not planned).
6. Free-threaded defaults: `io_threads` is 1 on a free-threaded Python for semantic safety (hello `def` is about 2.4x slower than with 4 threads): ask/decide whether to raise it.
7. CI: check it is green on all three OS (earlier failures: abi3 sha256 memoryview fixed; `test_the_command_line_takes_io_threads` fixed with PYTHONPATH; macOS spread assertion made Linux-only). The `listen()` hang was a race: one green run proves little.
8. Competitors (he asked): C++ Drogon, uWebSockets, libhv (speed), Crow, oat++, RESTinio, cpp-httplib, Boost.Beast (API); Python uvicorn (done), granian, Robyn. Drogon is now in the suite (ADR-0030); the others (uWebSockets, libhv, Crow, oat++, RESTinio, Boost.Beast, granian, Robyn) are not.
9. Small items: README gaps (when `def` vs `async def`, HTTPException, response classes, `httpp.status`, 405), `listen()` via `signal.set_wakeup_fd` (50 ms poll now), thread-safety review of module-level Python state under free-threading (`_tasks`, `_loop`, `_lanes`), FastAPI benchmark with `response_model` + dependencies, first batch of 8 blocking handlers is slower (0.49 s vs 0.21 s), Swagger UI body copied per request (changes a public type: ask), real zlib instead of the stb codecs (he said drop it for now; recommendation was madler/zlib as a pinned submodule), `--workers N` as threads on free-threaded builds, a total deadline for a dripped request body, optional callgrind of the I/O path.
10. Sub-interpreter experiment and the Starlette/FastAPI own-test-suite compatibility run (`docs/agent.md` section 6) remain from the earlier plan; priorities not confirmed.

Server gzip exists: `set_compression(min_size, level)` / `Server(compression=...)` (text answers only, off by default); it has no ADR yet and no benchmark. `checklist.md` line about the GZip middleware is stale.

## 8. Not verified (keep stating)
Windows and macOS runtime (the tunnel uses the same Asio calls; nothing run there), Autobahn, a real network (client and server share one machine), more than the 4 physical cores of the bench server, Python 3.15t locally (CI only), behaviour behind a TLS proxy (`wss`), memory under many idle WebSockets, the Python WebSocket glue under a long free-threaded stress, whether the Date race caused the lost servers.

## 9. Honest limitations to keep stating
Python handler code needs the GIL unless the interpreter is free-threaded or you use `--workers` (Linux only, no respawn, SIGKILL on the supervisor leaves workers). Free-threaded handlers must be thread-safe. Sandbox numbers (2 cores shared with the load generator) cannot show multi-core scaling.
