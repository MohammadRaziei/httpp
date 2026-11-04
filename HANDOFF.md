# httpp hand-off (consolidated)

This one file replaces handoff.md and handoff_5 to handoff_14. Read it first; the ADRs hold the reasoning. Older files can be deleted or kept for history.
Owner: Mohammad (GitHub `mohammadraziei`), the author of httpp. Date of this file: 2026-11-04 (sections 1 to 10 date from before; read sections 11 and 12 first for the latest state).

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
- ADRs 0001 to 0031 are in `docs/adr/` (index in its README). Recent: 0019 vendored deflate/inflate (gzip/deflate in the clients), 0020 HTTPException/response classes, 0021 pool/two-core numbers, 0022 dual API rule, 0023 inline async, 0024 lanes, 0025 measured defaults + the crash, 0026/0027 WebSocket.
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

## 10. Session 2026-10-31/11-01: file browser design review and compressed embeds
Nothing is committed or pushed (the owner commits). The working tree has these uncommitted changes on top of `d88781f`:
- **`embed_files(... COMPRESS)` and `-DHTTPP_EMBED_COMPRESS` (default ON), ADR-0031.** gzip made by CMake itself, MTIME zeroed (CMake writes the time, so builds differed). Declares `<name>_raw_size` and `<name>_is_gzip`. Tests: ctest `embed_compress_ON` and `embed_compress_OFF` (`tests/cmake/embed_compress/`).
- **Swagger UI now stored as gzip (owner asked for every embedded js and css).** Public type changed: `swagger_ui_asset::data` is the stored bytes, new `raw_size` and `gzip`. `mount_swagger_ui` sends the stored bytes with `Content-Encoding: gzip` and `Vary: Accept-Encoding` to clients that accept gzip, inflates for the others (no cache). `libhttpp_core.so` 4806064 to 3487152 bytes. Python `_swagger_ui_asset` returns a 3-tuple.
- ADR numbers are racing with upstream: mine was renumbered twice (0029, then 0030, both taken upstream). Check `docs/adr/` before writing the next one. **The file browser ADR is 0032.**
- Verified: C++ suite 384 passed (ON); `httpp_swagger*` 12 passed with ON and with OFF (full suite not run with OFF); `test_openapi.py` and `test_cpp_python_parity.py` 23 passed, 1 skipped (3.14, GIL). **The whole Python suite was not run** (stopped by the owner). Not run: Windows, macOS, 3.14t, 3.13, TSan, other CMake versions than 3.28.3.
- Rebased on origin/master three times with `git pull --rebase --autostash`; each time only the ADR index and CHANGELOG conflicted.

### File browser (read-only), design review NOT finished, no code written
The owner wants a read-only file browser (list, navigate, view, download; no write operations), with options (search and theme named), a CLI and a code API.
Inspiration: jsfb (Apache-2.0, `min.js` about 30 KB plus `min.css` about 23 KB, icons from Font Awesome or Bootstrap Icons, no network code, offers rename and delete in its menu) and a CodePen demo of it (list, grid and preview views, sort menu, info bar, a `..` row, mock JSON data).
Facts from the code: no ETag or Last-Modified exists (HANDOFF said it did); the router has no wildcard, so a deep path needs the `serve_directory` mount mechanism; nothing reads directories yet (no `opendir` or `readdir`), and `std::filesystem` is banned in `src/core`; `inside_dir` uses `realpath` on POSIX and treats a root of `/` as all-allowed.
Proposed, NOT yet agreed (the owner said "good" to the shape, and "continue" is still the way to accept all of it):
- Own small UI (HTML, JS, CSS, inline SVG icons, no dependency) embedded with `embed_files(... COMPRESS)`; the page is a template filled at mount time (theme, title, accent), as `mount_swagger_ui` does.
- `server::mount_file_browser(base, root, file_browser_options)`, Python `app.mount_file_browser(...)`, one design for both. Routes under `base`: page, `ui/<asset>`, `api/list?path=` (JSON: `path`, `entries[{name,type,size,mtime}]`, `truncated`, `limit`), `raw/<path>` (reuses the static file code: Range, HEAD, 206/416).
- Options: `title`, `theme` (auto, light, dark), `accent` (`#rrggbb` only), `search`, `show_hidden` (default false: hidden from the list and 404 on direct access), `max_entries` (5000, `truncated`), `view`, `sort`.
- The served root is the ceiling: the UI shows no `..` row in the root, a child folder can go up to the root only, the server never returns a parent, and any `..` segment (also `%2e%2e`), backslash, absolute path or symlink out of the root is refused (400 or 404, same body for "missing, hidden, outside").
- Security to add: active types (html, svg, xml, js, unknown) always `Content-Disposition: attachment` plus `X-Content-Type-Options: nosniff`; `filename*=UTF-8''` encoding; root `/` refused; Windows `:`, drive letters and reserved names refused (untestable here); no authentication in v1, the `before` hook is the way (document it).
- Listing runs on a pool thread (not the I/O thread); the scan stops at `max_entries`.
- CLI: new `httpp browse [DIRECTORY]` (`--host` default 127.0.0.1, `--port`, `--theme`, `--accent`, `--no-search`, `--hidden`, `--max-entries`, `--title`, `--view`, `--sort`); `httpp server` is unchanged.
- Open questions for the owner: `search` in v1 as a filter of the current folder (proposed) or also a recursive search (DoS risk, needs depth, count and time limits); ETag and 304 not in v1 (proposed); free function or `server` member (member proposed).
- Order after approval: tests first (traversal matrix, 405 for write methods, list limit, headers, parity test), then code, then ADR-0032.

## 11. Session 2026-11-01/02: v0.8.0 prepared, engine cost work (ADR-0033)
Nothing is committed or pushed (the owner commits). Version is 0.8.0 (`python version.py` shows it; set in `include/httpp.h`, `CHANGELOG.md` has `[0.8.0] - 2026-11-01` and `[Unreleased]` empty). Tagging with `python version.py tag create` is the owner's step (it commits).

### What was done
- **Drogon link fix** (`benchmarks/CMakeLists.txt`): the system static libcrypto.a pulled in undefined ZSTD and jitterentropy symbols. Drogon's OpenSSL discovery is switched off for the benchmark build (`CMAKE_DISABLE_FIND_PACKAGE_OpenSSL`, unset afterwards). Pins: Drogon v1.9.14 (latest stable), jsoncpp 1.9.6 (1.9.8 exists; the GCC 15 `-Wuninitialized` warning in jsoncpp and the deprecation warning inside Drogon are harmless third party warnings).
- **Why Drogon was faster**: not libuv (Drogon uses its own trantor on epoll). It was CPU per request in our layers. Measured with callgrind (difference of 2000 and 22000 oha requests, divided by 20000): route_io 25.5k to 18.3k instructions per request (Drogon about 14.6k).
- **ADR-0033 items**: Asio `ASIO_CONCURRENCY_HINT_UNSAFE_IO`; same-thread answer fast path (`thread_local t_loop_engine`); lazy deadline timers; response head built in one reserved string; `keep_alive_max` default 1000 and 0 = no limit (A/B knob in `cpp_bench`: third argument); small copies and tolower removed; **per-I/O-thread 504 wait lists** (`track`/`sweep` in `server.cpp`, `tl_wait_list`/`tl_wait_owner`), which fixed the shared mutex that made route_io scale worse than route.
- Tests added: `keep_alive_max_zero_means_no_limit` (C++), `{"keep_alive_max": 0}` case (Python), 504 on every I/O thread (C++, non-Windows).

### Verified
- C++ suite 386 tests pass and are TSan clean (whole suite). Python 653 passed, 4 skipped on 3.14 GIL. Autobahn (owner, CI): 589 OK, no FAILED.
- Owner's full bench earlier: 0 errors. Latest hello-only run (2026-11-02, tree 1628f86594, governor powersave, 3 rounds, 64 connections, 0 errors): C++ route_io 256k / 460k / 630k req/s for 1 / 2 / 4 I/O threads against Drogon 281k / 496k / 760k (7 to 17 percent behind; before the work 208k / 392k / 516k). py 3.14 `def` 241k / 402k / 521k; uvicorn 78k; FastAPI 22k. py 3.14 GIL `async def` goes down from 2 to 4 threads (345k to 309k, GIL contention); 3.14t `async def` 528k at 4.

### Not verified
Windows and macOS runtime, CI on the three OS (owner), Python 3.13 and 3.14t full suites after these changes, ThreadSanitizer on the Python layer, the file browser (not started).

### Guesses (not measured; say so when you use them)
- The remaining 7 to 17 percent gap to Drogon is probably shared atomic counters: `weak_ptr<config>::lock()` in `make_sender` (one control block touched by every I/O thread on every answer), plus shared_ptr copies of `responder::impl`, the `once` flag and the poster. shared_ptr counting was about 6 percent of cycles in the owner's earlier perf profile. Callgrind counts instructions, not cache line contention, so only `perf` on the owner's machine can confirm it.
- Plan if confirmed: a per-engine token that holds the `shared_ptr<config>`, with the same "server is gone" semantics; fewer shared_ptr copies per request; a timing wheel instead of lazy timers; build `request` straight from the llhttp state.
- The odd INLINE result (`async def INLINE` io=1 slower than worker-pool `async def`) is unexplained.

### Working routine that worked
1. Measure per-request instructions with callgrind (`cpp_bench` + oha), change one thing, re-measure (sandbox has 2 cores and no perf).
2. Check each change under TSan (separate build dir) and run the C++ and Python suites.
3. Send `httpp.zip`; the owner runs `bench` (hello only: `cmake -S benchmarks -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DBENCH_ONLY=hello -DBENCH_SECONDS=5 -DBENCH_ROUNDS=3 -DBENCH_IO_THREADS=1,2,4`, then `cmake --build build-bench --target bench`) or `perf record -e cpu_core/cycles/` and sends the result file. Beware a stale server sharing the port through SO_REUSEPORT (gave a bogus 892k once).
4. Put the honest numbers and the "not verified" part in ADR-0033 and CHANGELOG.
The owner delegated every performance decision to me ("any change in the project's interest"), only constraint: the C++ and Python APIs stay dual.

### Next steps
1. Owner: CI green on three OS, commit, `python version.py tag create`, push.
2. Optional 0.8.1: `perf` run on the owner's machine, then the `make_sender` / shared_ptr work above, re-measure with the same hello-only recipe.
3. Run the Python suite on 3.13 and 3.14t; bump `HTTPP_BENCH_JSONCPP_TAG` to 1.9.8 (test the benchmark build first).
4. File browser: design review still open (section 10); ADR number 0032 is reserved for it.

## 12. Session 2026-11-02 to 11-04: 0.8.1 performance work (ADR-0033 items 9 to 12, ADR-0034)
Nothing is committed or pushed (the owner commits; `python version.py tag create` is his step). Rules of section 2 apply unchanged (Persian in conversation, English with no em dash and no Persian in code, comments and commands in the repo, never push, send `httpp.zip` after every step). The owner delegated every performance decision ("only efficiency, speed and correct architecture matter"): decide, say why, record in an ADR.

### What changed in the tree (all uncommitted)
- **ADR-0033 item 9:** `make_sender` holds no `weak_ptr<config>` when there is no `after` hook, error handler or compression (the config is read-only once listening). `to_wire` is static.
- **ADR-0033 items 10 to 12** (instructions per `route_io` request 15425 to about 11.8k, callgrind): response head written with constant-size copies into one buffer of a known upper bound (no `strlen`, no append per piece); `ieq` with an inline length test, `header_ok` on string views; `response` and `wire_response` travel by rvalue reference (`send_fn`, `to_wire`, `answer_fn`, `conn::send`); one shared `answer_state` per request (one allocation, one count); a request already answered when its handler returns is not entered in the 504 list (`responder::impl::unwait` is an atomic function pointer published last with release, TSan found the race in the first version); `to_wire` takes the handler's header vector as it is and sends exactly one `Content-Type`, last. Three wire tests pin the Content-Type behaviour.
  Visible detail: the 504 deadline of an unanswered async request starts when its handler returns (it started at arrival).
- **ADR-0034:** with several I/O threads each accepted connection goes to the engine with the fewest connections (`async_server::balance_with`, `connections()`, `server::listen_after_bind` wires the engines together). `release()` from the accepting reactor, one `poster` hop, `assign()` in the target's `io_context`; a parked descriptor is closed if the target is shutting down. Two engine tests (exactly 16 each for 64 connections opened one after another; a handed-over connection serves requests and frees its slot).
- **`benchmarks/diag_io.sh`** (documented in `benchmarks/README.md`): perf with kernel samples, CPU ticks per server thread, counters per request; `BENCH_CPP_BIN=build-bench/drogon_bench BENCH_DIAG_PATH=/sync benchmarks/diag_io.sh` for Drogon. Writes into `build-bench/diag/<binary>/`. It sets `perf_event_paranoid=-1` and `kptr_restrict=0` through sudo. The syscall tracepoint of part 3 needs tracefs access and was refused on the owner's machine; the script now loses only that line.
- Tests: 391 C++ tests pass, whole suite clean under ThreadSanitizer (after the `unwait` fix). The Python suite was NOT run (core internals only, no public API change).

### Measurements (owner's machine, i7-13620H, server CPUs 0-7, load CPUs 8-15, oha 64 connections)
- Official hello-only bench, `route_io` req/s for 1 / 2 / 4 I/O threads: before the instruction work 265k / 489k / 659k (Drogon 272k / 564k / 812k); after it (tree db71804f60) 265k / 518k / 697k (Drogon 271k / 545k / 841k). One thread did not move (kernel dominates), 2 and 4 threads gained about 6 percent.
- `diag_io.sh` (single runs, NOT the interleaved bench) at 4 threads: before ADR-0034 httpp 690k to 705k with thread ticks 306/301/257/251 in 3 s; after ADR-0034 893k to 896k with 310/308/308/307. Drogon in the same runs 796k to 878k before and 706k to 818k after (noisy). **The official bench has not been re-run on the ADR-0034 tree. Do not claim parity with Drogon until it has.**
- Per-request cost at 4 threads (CPU time over requests): httpp about 5.5 us, Drogon about 4.55 us, before ADR-0034.

### Findings (what we know, what we do not)
- Both servers spend about 63 to 65 percent of their cycles in the kernel, in the same functions with the same shares (`tcp_*`, `skb_*`, conntrack). User space is about 36 percent for httpp, about 34 for Drogon. Cutting user instructions by 23 percent therefore gives only a few percent of throughput.
- Syscalls per request are at the minimum (strace, 8 connections: 1.0 `sendto`, 1.0 `recvfrom`, 0.25 `epoll_wait`, no EAGAIN `recv`).
- The real 4-thread problem was the uneven spread of connections by the `SO_REUSEPORT` hash (strace here: 17/15/9/9, 12/10/5/3; owner's ticks 306/301/257/251 against Drogon 301/301/301/296). Two of four threads sat about 16 percent idle in the closed loop. My first reading of the (buggy, awk field) tick output as "evenly spread" was wrong.
- netfilter (`nf_conntrack`, `nf_nat`, `nf_hook_slow`) takes about 5 percent of cycles on the owner's loopback for both servers.
- Still unexplained: whether httpp's kernel time per request stays higher than Drogon's (about 15 percent before ADR-0034) after the balance; `per_request.txt` (IPC, cache misses, migrations, context switches) never arrived for either server.
- `async def INLINE` at one I/O thread is slower than the worker-pool `async def` (181k to 189k against 233k to 240k): still unexplained. `async def` on 3.14 with the GIL drops from 2 to 4 I/O threads (about 399k to 307k to 319k): GIL contention, expected.

### Architecture against Drogon (I read `HttpServer.cc`, the response renderer and the profiles; I did NOT read trantor, its submodule was empty)
Same shape: N I/O loops with an epoll each, handlers allowed on the loop thread. Differences: Drogon deals connections out in turn (we now balance by load); handlers on the loop by default (our `route_io` is the equivalent, plain `route` and Python `def` take a hop to the pool); Drogon has its own small event loop (trantor), we use Asio (about 3 to 4 percent of cycles in scheduler and op objects: not worth replacing now); own parser against llhttp; timing wheel against lazy per-connection deadlines; about 9 KiB per idle connection against about 20 KiB (sandbox, unchecked). Our extra costs come from requirements Drogon does not have (dual C++ and Python API, answers from any thread, 504 backstop, hooks). No architecture change is indicated unless the official bench still shows a gap.

### Recipe that worked (sandbox, 2 cores, no perf, no oha)
Instructions per request: compile `benchmarks/cpp/cpp_bench.cpp` against `build/libhttpp_core.so`, run it under `valgrind --tool=callgrind` with `keep_alive_max` 0 and 1 I/O thread, drive 2000 and then 22000 requests of `/io` over 8 keep-alive connections with a small Python socket client, take (Ir 22000 minus Ir 2000) / 20000. `callgrind_annotate --tree=caller` for callers. Contention and kernel effects are invisible to callgrind: those need the owner's `diag_io.sh`. Connection spread: `strace -f -e trace=accept4` on `cpp_bench PORT 4`, count accepts per tid. TSan: separate build dir (recipe in section 5), run the whole suite.

### Next steps, in order
1. **Owner:** unzip, run the official bench on this tree: `cmake --build build-bench --target bench 2>&1 | tee bench_new2.log` (hello-only: see section 11 for the configure flags), send the report. Success looks like `route_io` at 4 threads near or above 850k with Drogon at its usual 810k to 880k, and balanced ticks in `diag_io.sh`.
2. **Owner:** commit (ADR-0033 and ADR-0034 and the CHANGELOG are ready; next version 0.8.1), CI on the three OS (the descriptor handover of ADR-0034 uses `release()` and `assign()`, which Asio supports on Windows and macOS, but nothing ran there).
3. If a gap remains: rerun `diag_io.sh` for both servers (part 3 gives IPC, cache misses, migrations), then look at allocations (about 13 per request: the request object, header vectors, `send_fn`, `responder::impl`, the write closure), `__tls_get_addr` (17 calls per request, `thread_local` in a shared library), building `request` straight from the llhttp state, and reading trantor (clone it separately) for the connection and buffer handling.
4. Check the suspected old use-after-free with ASan or a targeted test: in the hook path of `make_sender` the `job` lambda uses `this` (`pool`) after `weak.lock()` locked only `config`; if the server is destroyed at that moment `this` dangles.
5. Run the Python suites (3.14 GIL, 3.14t, 3.13) and TSan on the Python layer for everything above; check Windows and macOS.
6. Optional for fair benchmarking: take conntrack out of the loopback path (affects both servers equally, about 5 percent of cycles), and measure memory per idle connection (httpp about 20 KiB, Drogon about 9 KiB in the sandbox).
7. **File browser (ADR-0032 is reserved for it):** the design review of section 10 is still open; nothing is written.
8. Older open items stay as in section 7 (Autobahn after the push, the free-threaded defaults, README gaps, competitors, and so on).

### Not verified (keep stating)
The official-bench effect of ADR-0034, Windows and macOS, the Python suite and Python layer under TSan after all of the above, free-threaded behaviour, whether kernel time per request still differs from Drogon's, the descriptor handover under heavy connection churn (one extra hop per new connection; not measured).
