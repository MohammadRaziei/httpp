# httpp: Checklist

> Marks: `[x]` done and verified, `[ ]` not done, `[~]` partly done. Measurement conditions are next to every number.
> The sandbox that produced most measurements has 2 cores and shares them with the load generator; multi-core numbers come from the owner's machine only (see `HANDOFF.md` section 4).
> Accepted decisions are in [docs/adr](docs/adr/README.md). Items marked **(proposed)** are suggestions that the owner has not agreed to yet.
> Last update: 2026-11-05.

---

## Current status (built and verified)

- [x] Own async server engine (Asio + llhttp): keep-alive, pipelining, chunked, limits (413/414/431); connections balanced over I/O threads ([ADR-0034](docs/adr/0034-balanced-connections-between-io-threads.md))
- [x] Server on top of it: router, static files with HEAD/Range, hooks, `route_async` with a 504 backstop, `route_io`/`route_shared`
- [x] Async client engine (TLS with mbedTLS, keep-alive pool, proxy, redirects, cookies/auth); sync client and downloads on cpp-httplib
- [x] FastAPI-like layer: `Query/Path/Header/Cookie/Body/Depends`, forms and uploads, OpenAPI 3.1 + compiled-in Swagger UI, response classes, `HTTPException`, `BackgroundTasks`, `APIRouter`, startup/shutdown hooks ([ADR-0007](docs/adr/0007-openapi-from-one-plan.md), [0008](docs/adr/0008-swagger-ui-compiled-in.md), [0020](docs/adr/0020-httpexception-and-response-classes.md))
- [x] Own Pydantic-v2-shaped models with Cython validators ([ADR-0006](docs/adr/0006-own-model-layer.md))
- [x] `httpp run main:app`, `app.run()`, `--workers N` (Linux), `io_threads`, lanes, inline async, free-threaded builds ([ADR-0012](docs/adr/0012-run-wins-over-listen.md), [0023](docs/adr/0023-inline-async.md), [0024](docs/adr/0024-io-threads-and-lanes.md), [0025](docs/adr/0025-measured-defaults-and-the-crash.md))
- [x] WebSocket: C++ engine tunnel and RFC 6455 session, Python `@app.websocket`; Autobahn 589 OK, no FAILED ([ADR-0026](docs/adr/0026-websocket-engine.md), [0027](docs/adr/0027-websocket-handshake-and-python.md), [0029](docs/adr/0029-utf8-checked-per-fragment.md))
- [x] Logging bridge, compiled and compressed embedded files ([ADR-0031](docs/adr/0031-compressed-embedded-files.md)), vendored gzip/deflate in the clients ([ADR-0019](docs/adr/0019-vendored-deflate-inflate.md))
- [x] Read-only file browser: C++ `mount_file_browser`, Python `Server.mount_file_browser`, `httpp browse` ([ADR-0032](docs/adr/0032-read-only-file-browser.md))
- [x] Engine cost work: [ADR-0033](docs/adr/0033-engine-per-request-cost.md) (with the 2026-11-05 addendum), [0035](docs/adr/0035-hook-path-needs-no-server-pointer.md), [0036](docs/adr/0036-connection-read-buffer-not-zero-filled.md). Against Drogon on 4 I/O threads the two are now roughly equal (medians 652k against 618k req/s, ranges overlap; see `HANDOFF.md` section 5)
- [x] Tests (2026-11-05): 407 C++ tests, clean under ASan and TSan; Python 665 passed / 4 skipped (3.14 GIL), 664 / 5 (3.14t), 665 / 4 (3.13)
- [ ] CI green on Linux, macOS and Windows for the current tree (owner; earlier runs were green, nothing newer is confirmed)

---

## 1. Command line in C++ (next, owner's design)

Design and costs: `HANDOFF.md` section 6.1. Rules: tests first; behaviour identical to today's `__main__.py`; English only, no em dash; zero system dependencies.

- [ ] Tests: extend `tests/python/test_cli.py` (today `browse` is covered in `test_file_browser.py`): `server`, `download`, `curl`, `--help`, exit codes, argv with spaces and quotes passed through the shim unchanged, `run` delegation
- [ ] Vendor `CLI11.hpp` (v2.5.0, single header, unmodified) in `src/third_party/CLI11/`; add it to `src/third_party/README.md` (version, license, "unmodified")
- [ ] `src/cli/httpp_cli.cpp`: subcommands `server`, `browse`, `download`, `curl`, `run` (delegates to `$HTTPP_PYTHON -m httpp._run_cli`), `path`/`install`/`uninstall` (decision below)
- [ ] CMake: executable target with `OUTPUT_NAME httpp`, linked to `httpp_core`, installed with the SDK and into the wheel inside the package folder (`httpp/_bin/`), with a non-Windows and a Windows (`.exe`) path; macOS 10.13 limits apply (POSIX calls, no `std::filesystem`)
- [ ] `httpp/__main__.py`: only locate the binary, set `HTTPP_PYTHON=sys.executable`, `subprocess.run([binary, *argv])`, return its exit code; a clear error when the binary is missing; the `httpp` console script stays
- [ ] `httpp/_run_cli.py` (or reuse `_run.py`): the Python half of `run`, same flags as today
- [ ] Decide with the owner: `path`, `install`, `uninstall` in Python (proposed) or in C++
- [ ] Wheel CI: the executable is present and runs in every wheel (`cp39-abi3`, `cp313t`, `cp314t`, `cp315t`) on three OS
- [ ] ADR-0037, CHANGELOG, README (`httpp browse`, `httpp server` run without the GIL)

## 2. Features

### Server and framework
- [ ] **(proposed)** A test client in Python (`httpp.testclient`), Starlette-shaped, running the app in process without a port: needed for users' own tests
- [~] Middleware set (ADR-0037): CORS, TrustedHost and GZip done (C++ and Python); HTTPS redirect waits for server TLS (the server has no request scheme yet)
- [ ] **(proposed)** TLS for the server (`https`, `wss`): the engine has no TLS; mbedTLS is already vendored for the client; needs its own design and an ADR
- [ ] **(proposed)** `--reload` for development
- [ ] **(proposed)** Graceful shutdown with connection drain and a timeout; `--workers` respawn of crashed workers (today no respawn, and SIGKILL of the supervisor leaves workers)
- [ ] Dependencies with `yield` (cleanup after the response) and `response_model` filtering: check what the FastAPI layer lacks against FastAPI's docs
- [ ] Static files mounted from Python (`mount_static`-style) beyond `serve_directory`; ETag, `Last-Modified` and 304 (not in the file browser v1 either)
- [ ] A total deadline for a dripped request body
- [ ] Swagger UI body is copied per request (changes a public type; ask the owner first)
- [ ] The 504 deadline of an unanswered async request starts when the handler returns (document or change)

### WebSocket
- [ ] `APIRouter.websocket`, dependencies on WebSocket routes, an OpenAPI note
- [ ] `on_close` earlier than the lingering close (considered, kept), permessage-deflate (not planned)
- [ ] The WebSocket benchmark and idle-connection memory (`BENCH_ONLY=ws,idle`) were written (ADR-0030) but never run on the owner's machine; httpp is about 20 KiB per idle connection against Drogon about 9 KiB in the sandbox (unchecked): look for per-connection buffers

### File browser
- [ ] Recursive search with depth, count and time limits (the first version only filters the open folder)
- [ ] Windows branch of `file_browser.cpp` (never compiled): drive letters, `:` and reserved names
- [ ] Documentation of authentication through the `before` hook (README mentions it; add an example)

### Clients and downloads
- [ ] `Client(keep_alive=False)` Python binding decision; sync client against async client (two engines for one behaviour; separate decision)
- [ ] MIME types: `serve_directory` (C++ table) and `file()` (Python `mimetypes`) disagree for `ico`, `gz`, `wav`, `yaml`, `yml`; fix needs one new public function
- [ ] Real zlib instead of the vendored stb codecs (owner said drop it for now; recommendation if it returns: madler/zlib as a pinned submodule)

### Python layer
- [ ] `listen()` via `signal.set_wakeup_fd` instead of the 50 ms poll
- [ ] Thread-safety review of module-level state under free-threading (`_tasks`, `_loop`, `_lanes`)
- [ ] `--workers N` as threads on free-threaded builds
- [ ] Free-threaded default `io_threads` (1 today for semantic safety; hello `def` is about 2.4 times slower than with 4): decide
- [ ] ASGI adapter (low priority), sub-interpreter experiment, running Starlette's and FastAPI's own test suites against httpp (`docs/agent.md` section 6)

## 3. Performance (optional, small; the owner called the Drogon gap closed enough)
- [ ] Official interleaved bench on the current tree (needs the owner's `uv` to reach PyPI; try a proxy or VPN, `UV_HTTP_TIMEOUT=600`)
- [ ] A ten-round A/B script (medians, min, max) for httpp against Drogon, and the `performance` governor
- [ ] Fewer `shared_ptr` copies per request (about 14 releases; ceiling about 2 percent; needs ASan, TSan and the outlive tests)
- [ ] 3.14t `def` pool should start at 2 (+17 percent at 1 I/O thread, slower first burst), `def JSON` plateau on 3.14t, non-monotone inline on 3.14t
- [ ] Competitors: uWebSockets, libhv, Crow, oat++, RESTinio, Boost.Beast, granian, Robyn (Drogon and uvicorn are done)
- [ ] Optional: take conntrack out of the loopback path for fair benchmarking (about 5 percent of cycles for both servers)

## 4. Quality and release
- [ ] Windows and macOS runtime: nothing of the server, the tunnel, the descriptor handover (ADR-0034) or the file browser has run there
- [ ] ASan and TSan on the Python extension (only the C++ suite was run)
- [ ] The ASan leak report at exit (about 18 KB in `async_http::impl` exchanges alive at exit); the open `worker_pool*` read in ADR-0035
- [ ] Python 3.15t locally (CI only so far)
- [ ] README gaps: when to use `def` against `async def`, `HTTPException`, response classes, `httpp.status`, 405; the GZip section
- [ ] An ADR and a benchmark for server gzip (`set_compression`)
- [ ] Commit, `python version.py tag create` (version 0.8.1), push: owner's step; the CHANGELOG `[Unreleased]` section is ready

## 5. Binding policy (ADR-0017), unchanged
Python binds only what `include/` exports; a public C++ function is bound only when necessary; `tests/python/test_cpp_python_parity.py` forces every public C++ name to be bound or listed in `NOT_EXPOSED` with a reason. Not exposed on purpose: `route_io`, `route_shared`, `route_ws`, `find_swagger_ui_asset`. The new `mount_file_browser` is bound (as `Server.mount_file_browser`).
