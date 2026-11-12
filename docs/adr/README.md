# Architecture Decision Records

Each file records one architectural decision with its **context, decision, rejected alternatives and consequences**. They were extracted from
`docs/Architecture.md`, `handoff.md` and the real code; wherever something is not run or verified yet, the ADR says so.
Things that CI runs and passes are written as facts; skipped tests are listed in ADR-0015.

| ADR | Decision | Status |
|---|---|---|
| [0001](0001-own-both-layers.md) | httpp owns both layers (replacing FastAPI/Starlette and uvicorn) | accepted |
| [0002](0002-standard-asyncio-loop.md) | `async def` handlers run on the standard asyncio loop | accepted |
| [0003](0003-engine-and-third-party.md) | Asio + llhttp + Mbed TLS; cpp-httplib only for the sync client | accepted |
| [0004](0004-zero-system-dependencies.md) | No system dependencies | accepted |
| [0005](0005-multiprocess-workers.md) | Fresh processes + `SO_REUSEPORT` (Linux) | accepted (respawn part superseded by 0044) |
| [0006](0006-own-model-layer.md) | Pydantic-v2-shaped models with Cython validators | accepted |
| [0007](0007-openapi-from-one-plan.md) | OpenAPI from the same request plan | accepted |
| [0008](0008-swagger-ui-compiled-in.md) | Swagger UI compiled into the library | accepted |
| [0009](0009-hot-path.md) | Hot path: dispatch on the I/O thread, one timer, an indexed router | accepted |
| [0010](0010-mbedtls-threading.md) | Mbed TLS thread safety | accepted |
| [0011](0011-logging.md) | Request and bind-address logs in C++; defaults | accepted |
| [0012](0012-run-wins-over-listen.md) | `httpp run` wins over a `listen` inside the file | accepted |
| [0013](0013-packaging.md) | `cp39-abi3` + a separate wheel per free-threaded version | accepted |
| [0014](0014-coding-conventions.md) | STL-style `snake_case`, ponytail style, English only, no em dash | accepted |
| [0015](0015-testing-and-ci.md) | What is tested where; the list of skips | accepted |
| [0016](0016-hybrid-api-parity.md) | Hybrid C++/Python API parity: inventory of differences | partly superseded by 0017 |
| [0017](0017-python-binding-policy.md) | Python binding policy: speed decides, only public headers, only what is necessary | accepted |
| [0018](0018-python-logging-bridge.md) | The C++ log goes through Python `logging`; the logger controls the C++ level | accepted |
| [0019](0019-vendored-deflate-inflate.md) | gzip and deflate on vendored stb code, no system zlib | accepted |
| [0020](0020-http-exception-and-response-classes.md) | HTTPException and response classes (Starlette names, native) | accepted |
| [0021](0021-pool-thread-state-and-two-core-numbers.md) | Pool threads keep their Python thread state; first two-core numbers incl. FastAPI | accepted |
| [0022](0022-dual-api-rule-and-delegated-decisions.md) | Dual C++/Python rule; decisions the owner delegated | accepted |
| [0023](0023-inline-async-handlers.md) | `async def` handlers that cannot suspend run on the I/O thread (gate, busy count) | accepted |
| [0024](0024-io-threads-and-lanes.md) | Several I/O threads; one asyncio loop per I/O thread on a free-threaded Python | accepted |
| [0025](0025-measured-defaults-on-16-threads.md) | Defaults after the first 16-thread measurement (`io_threads`, inline `async def`) | accepted |
| [0026](0026-websocket.md) | WebSocket (RFC 6455) in the C++ core, on the I/O thread; bounded send queue | accepted (C++ core; handshake amended by 0027) |
| [0027](0027-websocket-python-and-async-handshake.md) | WebSocket in Python, asynchronous handshake, bounded mailboxes | accepted |
| [0028](0028-autobahn-called-by-the-orchestrator.md) | The orchestrator calls the Autobahn workflow (ordering with the `__beta__` clean-up) | accepted |
| [0029](0029-incremental-utf8-validation.md) | Text messages are checked for UTF-8 as the bytes arrive (Autobahn 6.4.x) | accepted |
| [0030](0030-benchmarks-built-by-cmake.md) | The benchmarks are a CMake project; WebSocket and Drogon join the suite | accepted |
| [0031](0031-compressed-embedded-files.md) | Embedded files can be gzip-compressed at configure time (`embed_files(... COMPRESS)`, `HTTPP_EMBED_COMPRESS`) | accepted |
| [0032](0032-read-only-file-browser.md) | A read-only file browser (`mount_file_browser`, `serve_directory` options, own embedded UI) | accepted (C++ core) |
| [0033](0033-engine-per-request-cost.md) | Fewer instructions per request: no reactor locking, no hop for answers made on the loop thread | accepted |
| [0034](0034-balanced-connections-between-io-threads.md) | Connections are shared evenly between I/O threads (least loaded engine takes the next one) | accepted |
| [0035](0035-hook-path-needs-no-server-pointer.md) | The answer hook path does not touch the server object (late answers after the server is gone) | accepted |
| [0036](0036-connection-read-buffer-not-zero-filled.md) | The per-connection read buffer is not zero-filled | accepted |
| [0037](0037-middleware-through-use.md) | Middleware through `server::use()` and ready-made hooks (CORS, trusted host, gzip) | accepted |
| [0038](0038-graceful-shutdown.md) | Graceful shutdown with a grace period (`shutdown()`, SIGTERM, `--shutdown-timeout`) | accepted |
| [0039](0039-reload-supervisor.md) | `--reload`: a polling supervisor that restarts the app process | accepted |
| [0040](0040-server-tls.md) | Server TLS (https, wss) on the engine with the vendored Mbed TLS | accepted |
| [0041](0041-native-cli-and-py.md) | The command line is a native executable; Python commands live behind `py` | accepted |
| [0042](0042-test-client.md) | `httpp.testclient.TestClient` serves the app on a loopback port | accepted |
| [0043](0043-websocket-close-1001-on-drain.md) | WebSocket connections get a close 1001 frame when the server drains | accepted |
| [0044](0044-worker-respawn.md) | The `--workers` supervisor restarts dead workers and workers die with it | accepted |
| [0045](0045-conditional-static-files.md) | Files from a mounted directory carry ETag and Last-Modified and answer conditional requests | accepted |
| [0046](0046-testclient-websocket-and-files.md) | `TestClient.websocket_connect` and multipart `files=` | accepted |
| [0047](0047-one-mime-table-and-exchange-leak.md) | One MIME table for the C++ and Python servers, and the exchange leak of `async_http` | accepted |
| [0048](0048-websocket-on-routers-and-dependencies.md) | WebSocket routes on routers, and dependencies on WebSocket routes | accepted |
| [0049](0049-split-the-cython-module-into-pxi-files.md) | Split the Cython module into `.pxi` files | accepted |
| [0050](0050-own-logging-module-replaces-aixlog.md) | An internal logging module (ported from libstb) replaces AixLog | accepted |
| [0051](0051-sync-client-on-the-async-engine.md) | The blocking client runs on the async engine and one shared policy layer | accepted |
| [0052](0052-downloads-stream-through-a-body-sink.md) | Downloads stream through a body sink on their own engine | accepted |
| [0053](0053-cpp-httplib-removed.md) | cpp-httplib is removed | accepted |
| [0054](0054-own-file-system-module.md) | An own file system module, `httpp::fs`, replaces std::filesystem | accepted |
| [0055](0055-httpp-path-class.md) | `httpp::path`, a pathlib-shaped public path type | accepted |
| [0056](0056-operating-system-log-handlers.md) | The operating system log handlers of libstb, behind `set_log_system` | accepted |
| [0057](0057-public-logging-module.md) | The logging module is public: `httpp/logging.hpp` | accepted, amends 0050 |
| [0058](0058-fs-merged-into-path.md) | The file system module is merged into `httpp::path` | accepted, amends 0054 and 0055 |
| [0059](0059-free-threaded-pool-starts-at-two.md) | The worker pool of a free-threaded Python starts at two threads | accepted, amends 0025 |
| [0061](0061-server-gzip-defaults.md) | Server gzip stays off by default, level 1, and its cost is documented | accepted |

## Not decided yet

These have no ADR because they are not decisions yet. They are tracked in `docs/Architecture.md` section 7 and must be decided **with the project owner**:
the shared memory library and the scope of `app.shared`, the coupling level, the exact API scope (Starlette-like, or FastAPI-like as well),
removing the hop for `async def` handlers that await (see ADR-0023), WebSocket backpressure, the sub-interpreter experiment, a lighter replacement for Swagger UI.

## Writing a new ADR

File name `NNNN-short-english-title.md`. Sections: status, context, decision, rejected alternatives, consequences (including what is **not** verified), related.
Do not edit an accepted ADR; when a decision changes, write a new ADR and mark the old one "superseded by ADR-NNNN".
