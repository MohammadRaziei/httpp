# Architecture Decision Records

Each file records one architectural decision with its **context, decision, rejected alternatives and consequences**. They were extracted from
`docs/Architecture.md`, `handoff.md` and the real code; wherever something is not run or verified yet, the ADR says so.
Things that CI runs and passes are written as facts; skipped tests are listed in ADR-0015.

Subjects that were decided in many small steps are **merged ADRs**: one file per subject, with a "Current state" summary on top and the original
texts kept, in order, as its parts. Numbers are not renumbered, so there are gaps; a number that was merged away is not used again.

| ADR | Decision | Status |
|---|---|---|
| [0001](0001-own-both-layers.md) | httpp owns both layers (replacing FastAPI/Starlette and uvicorn) | accepted |
| [0002](0002-standard-asyncio-loop.md) | `async def` handlers run on the standard asyncio loop | accepted |
| [0003](0003-engine-and-third-party.md) | Asio + llhttp + Mbed TLS; cpp-httplib only for the sync client | accepted |
| [0004](0004-zero-system-dependencies.md) | No system dependencies | accepted |
| [0005](0005-multiprocess-workers.md) | Fresh processes + `SO_REUSEPORT` (Linux) | accepted (respawn part superseded by 0044) |
| [0006](0006-own-model-layer.md) | Pydantic-v2-shaped models with Cython validators | accepted |
| [0007](0007-openapi-and-swagger.md) | OpenAPI and Swagger UI: one request plan, one C++ writer, Swagger UI compiled in, `swagger` as a mountable | accepted |
| [0009](0009-hot-path.md) | Hot path: dispatch on the I/O thread, one timer, an indexed router | accepted |
| [0010](0010-mbedtls-threading.md) | Mbed TLS thread safety | accepted |
| [0011](0011-logging.md) | Logging: one public module, one level type, one logger for the library, a bridge to Python | accepted |
| [0012](0012-run-wins-over-listen.md) | `httpp run` wins over a `listen` inside the file | accepted |
| [0013](0013-packaging.md) | `cp39-abi3` + a separate wheel per free-threaded version | accepted |
| [0014](0014-coding-conventions.md) | STL-style `snake_case`, ponytail style, English only, no em dash | accepted |
| [0015](0015-testing-and-ci.md) | What is tested where; the list of skips | accepted |
| [0016](0016-dual-api.md) | The dual C++/Python API: the parity goal, the binding policy and the delegated decisions | accepted |
| [0019](0019-vendored-deflate-inflate.md) | gzip and deflate on vendored stb code, no system zlib | accepted |
| [0020](0020-http-exception-and-response-classes.md) | HTTPException and response classes (Starlette names, native) | accepted |
| [0021](0021-pool-thread-state-and-two-core-numbers.md) | Pool threads keep their Python thread state; first two-core numbers incl. FastAPI | accepted |
| [0023](0023-inline-async-handlers.md) | `async def` handlers that cannot suspend run on the I/O thread (gate, busy count) | accepted |
| [0024](0024-io-threads-and-defaults.md) | I/O threads, lanes, and the measured thread defaults | accepted |
| [0026](0026-websocket.md) | WebSocket: a generic tunnel in the engine, an asynchronous handshake, close 1001 on drain, routers and dependencies | accepted |
| [0028](0028-autobahn-called-by-the-orchestrator.md) | The orchestrator calls the Autobahn workflow (ordering with the `__beta__` clean-up) | accepted |
| [0029](0029-incremental-utf8-validation.md) | Text messages are checked for UTF-8 as the bytes arrive (Autobahn 6.4.x) | accepted |
| [0030](0030-benchmarks-built-by-cmake.md) | The benchmarks are a CMake project; WebSocket and Drogon join the suite | accepted |
| [0031](0031-compressed-embedded-files.md) | Embedded files can be gzip-compressed at configure time (`embed_files(... COMPRESS)`, `HTTPP_EMBED_COMPRESS`) | accepted |
| [0032](0032-file-browser.md) | The file browser and the directory server: read-only browsing, a builder class, a sandbox for html and svg, hidden names and file times | accepted |
| [0033](0033-engine-per-request-cost.md) | Fewer instructions per request: no reactor locking, no hop for answers made on the loop thread | accepted |
| [0034](0034-balanced-connections-between-io-threads.md) | Connections are shared evenly between I/O threads (least loaded engine takes the next one) | accepted |
| [0035](0035-hook-path-needs-no-server-pointer.md) | The answer hook path does not touch the server object (late answers after the server is gone) | accepted |
| [0036](0036-connection-read-buffer-not-zero-filled.md) | The per-connection read buffer is not zero-filled | accepted |
| [0037](0037-middleware-through-use.md) | Middleware through `server::use()` and ready-made hooks (CORS, trusted host, gzip) | accepted |
| [0038](0038-graceful-shutdown.md) | Graceful shutdown with a grace period (`shutdown()`, SIGTERM, `--shutdown-timeout`) | accepted |
| [0039](0039-reload-supervisor.md) | `--reload`: a polling supervisor that restarts the app process | accepted |
| [0040](0040-server-tls.md) | Server TLS (https, wss) on the engine with the vendored Mbed TLS | accepted |
| [0041](0041-command-line.md) | The command line: a native executable, commands that only call the library, and HTTPS | accepted |
| [0042](0042-test-client.md) | `httpp.testclient.TestClient`: a real server on a loopback port, with `websocket_connect` and multipart `files=` | accepted |
| [0044](0044-worker-respawn.md) | The `--workers` supervisor restarts dead workers and workers die with it | accepted |
| [0045](0045-conditional-static-files.md) | Files from a mounted directory carry ETag and Last-Modified and answer conditional requests | accepted |
| [0047](0047-one-mime-table-and-exchange-leak.md) | One MIME table for the C++ and Python servers, and the exchange leak of `async_http` | accepted |
| [0049](0049-split-the-cython-module-into-pxi-files.md) | Split the Cython module into `.pxi` files | accepted |
| [0051](0051-cpp-httplib-retired.md) | cpp-httplib is retired: the blocking client on the async engine, downloads through a body sink | accepted |
| [0054](0054-path-and-file-system.md) | Paths and the file system: no `std::filesystem`, one public `httpp::path` | accepted |
| [0060](0060-docs-site.md) | The documentation site: one page per concept, a Python/C++ code switch, published to GitHub Pages | proposed (waiting for the first deploy and the owner's review) |
| [0061](0061-server-gzip-defaults.md) | Server gzip stays off by default, level 1, and its cost is documented | accepted |
| [0062](0062-request-names-match-python.md) | The C++ request names match Python's: `httpp::request` is the client builder, `httpp::server_request` is the handler's request | accepted (rename done and checked on Linux, see Consequences) |
| [0063](0063-benchmark-json-and-docs-page.md) | The benchmark suite writes JSON; the docs site draws the charts | proposed |
| [0066](0066-option-structs-become-builders.md) | Middleware, directory and Swagger UI become builder classes (`cors`, `trusted_host`, `https_redirect`, `directory`, `swagger_ui`) | accepted |
| [0067](0067-url-and-proxy.md) | URLs: every URL parameter is an `httpp::url` (an alias of `urlparser::url`), and a proxy is one URL | accepted |
| [0068](0068-socks5-proxy.md) | SOCKS5 proxies (`socks5://`, `socks5h://`) through the same proxy URL | accepted |
| [0069](0069-forwarded-headers.md) | `forwarded_headers`: the real client, scheme and host from a trusted reverse proxy; `server_request::client_ip` | accepted |
| [0071](0071-client-pool-keeps-every-connection-of-a-burst.md) | The client pool keeps every connection of a burst (idle limit 8 to 256): `httpp.Client` scaling on free-threaded Python | accepted |
| [0074](0074-callback-handler-can-run-unserialized.md) | `callback_handler(fn, serialized = false)` calls `fn` without the handler's lock, so a callback that waits for the Python GIL cannot deadlock | accepted |
| [0077](0077-curl-command.md) | `httpp curl`: a curl-compatible command that only calls `httpp::request` | accepted |
| [0079](0079-request-redirects-timeout-compression.md) | `httpp::request`: `max_redirects` is the one redirect setting, plus `connect_timeout` and `auto_compression` | accepted |
| [0081](0081-server-is-a-shared-handle.md) | `server` is a copyable handle that shares its state; `mount` takes and returns it by value; `server_stopper` breaks handler cycles | accepted |
| [0086](0086-httpp-utils-base-library.md) | `httpp_utils`: a shared base library (logging, path, sformat, progress) below `httpp_core`; one logger registry per process; two export macros; core file is `libhttpp` | accepted |
| [0092](0092-settings-names.md) | Setting names: no `set_` prefix, the same names in C++ and Python, keywords as setter names | accepted / implemented |
| [0093](0093-client-handles-and-settings-mixin.md) | Client classes: shared handles with `clone()`, one settings mixin (`configurable`), `download` on the mixin, and `request` / `server` clones | implemented (the points marked "Taken by default" in the parts are for the owner to confirm) |
| [0095](0095-limit-and-unlimited-for-no-limit-settings.md) | a named "no limit" (`httpp::unlimited`) for `max_redirects`, `max_time` and `max_response_size`; the type and the Python spelling are open | proposed |
| [0097](0097-library-elapsed-retry-and-save.md) | The library does what `httpp curl` used to do itself: elapsed time, retry, and saving to a file | implemented in C++ (built and tested on Linux); each part says whether Python is bound (merged) |
| [0106](0106-direct-scheme-is-no-proxy.md) | `direct://` (any case, nothing after the scheme) means no proxy at all, like `""`, when a proxy is given explicitly: `proxy("direct://")`, `-x direct://`, `resolve_proxy(..., "direct://")`; an error in the environment; bare `direct` and `off` stay hosts | implemented (C++) |
| [0107](0107-environment-variables.md) | `httpp::env` in `httpp_utils`: `get`, `first`, `set`, `unset` on UTF-8 text (wide API on Windows) with one lock, and `env::scope`, which puts the old value back; the four `getenv` readers and the proxy test helper move onto it | implemented in C++ (built and tested on Linux); Windows branch not run |
| [0108](0108-resource-limits-and-request-checks.md) | Limits and request checks from the 0.9.0 security review: 1024 pairs and a 1 MiB form body, 10000 connections, one `Host`, loopback `Host` allowlist in the CLI, same-origin zip POST, no control or direction characters in multipart names, TLS needs a SAN, client body 256 MiB and head 64 KiB | implemented (C++, Python) |
| [0110](0110-waits-take-a-fraction-of-a-second.md) | `timeout`, `connect_timeout`, `max_time` and `retry_delay` are `double` seconds: a fraction is a real wait, in C++ and Python |
| [0111](0111-port-0-for-the-native-commands.md) | `httpp server` and `httpp browse` accept `--port 0`; the CLI tests start a server on port 0 and read the port from the printed line, so they are safe under pytest-xdist |
| [0112](0112-a-stopped-server-must-not-be-in-a-reference-cycle.md) | Handlers and hooks hold a shared `_RequestState` / `_OpenAPIDocument`, not the Server: a stopped server is freed by reference counting, also on free-threaded CPython; the 100-request test uses one client thread because a thread costs 44 to 75 MiB of address space there |
| [0113](0113-client-uploads-streamed-bodies-and-multipart.md) | Client uploads: `field`, `file`, `file_data`, `body_file` and `progress` on `request`, a body read while it is sent, `curl -F` and a streamed `-T`; no `upload` class |
| [0109](0109-download-on-retry-and-the-download-retry-flags.md) | `download::on_retry` and `httpp download --retry/--retry-delay/--retry-connrefused` (also without `-o`); one shared warning function with `curl`; `-q` hides it | implemented (C++) |

## Not decided yet

These have no ADR because they are not decisions yet. They are tracked in `docs/Architecture.md` section 7 and must be decided **with the project owner**:
the shared memory library and the scope of `app.shared`, the coupling level, the exact API scope (Starlette-like, or FastAPI-like as well),
removing the hop for `async def` handlers that await (see ADR-0023), WebSocket backpressure, the sub-interpreter experiment, a lighter replacement for Swagger UI.

## Writing a new ADR

File name `NNNN-short-english-title.md`; the next free number is **0114** (raise it when you use it). Sections: status, context, decision, rejected alternatives,
consequences (including what is **not** verified), related. Add the row to the table above in number order.
Do not edit an accepted ADR; when a decision changes, write a new ADR and mark the old one "superseded by ADR-NNNN", or, when the subject already has a merged ADR, see below.

## Merging ADRs

When one subject has grown over several ADRs that amend or supersede each other, merge them into one file:

- The merged file keeps the lowest number of its parts and gets a name for the subject. It starts with a "Current state" summary, then holds every part unchanged
  (a `###` heading with the part's title, its own headings moved two levels down), in the order they were decided.
- The other numbers and files are deleted, and every reference to them (code comments, docs, changelog, handoff) is changed to the merged ADR number. A reference
  between two parts of the same merged ADR names the part by its title.
- Never renumber the other ADRs and never keep a table of old numbers.
