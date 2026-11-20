# Architecture Decision Records

Each file records one architectural decision with its **context, decision, rejected alternatives and consequences**. They were extracted from
`docs/Architecture.md`, `handoff.md` and the real code; wherever something is not run or verified yet, the ADR says so.
Things that CI runs and passes are written as facts; skipped tests are listed in ADR-0015.

Several subjects were decided in many small steps and had spread over many numbers. They are now **merged ADRs**: one file per subject, with a
"Current state" summary on top and the original ADRs kept, in order, as its parts. Old numbers still work: code comments, the handoff and the
changelog that say "ADR-0057" mean the part with that number inside the merged file (see "Merged ADRs" at the end).

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
| [0011](0011-logging.md) | Logging: one public module, one level type, one logger for the library, a bridge to Python | accepted (merged: 0011, 0018, 0050, 0056, 0057, 0075, 0080, 0085, 0090, 0091) |
| [0012](0012-run-wins-over-listen.md) | `httpp run` wins over a `listen` inside the file | accepted |
| [0013](0013-packaging.md) | `cp39-abi3` + a separate wheel per free-threaded version | accepted |
| [0014](0014-coding-conventions.md) | STL-style `snake_case`, ponytail style, English only, no em dash | accepted |
| [0015](0015-testing-and-ci.md) | What is tested where; the list of skips | accepted |
| [0016](0016-hybrid-api-parity.md) | Hybrid C++/Python API parity: inventory of differences | partly superseded by 0017 |
| [0017](0017-python-binding-policy.md) | Python binding policy: speed decides, only public headers, only what is necessary | accepted |
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
| [0051](0051-sync-client-on-the-async-engine.md) | The blocking client runs on the async engine and one shared policy layer | accepted |
| [0052](0052-downloads-stream-through-a-body-sink.md) | Downloads stream through a body sink on their own engine | accepted |
| [0053](0053-cpp-httplib-removed.md) | cpp-httplib is removed | accepted |
| [0054](0054-path-and-file-system.md) | Paths and the file system: no `std::filesystem`, one public `httpp::path` | accepted (merged: 0054, 0055, 0058) |
| [0059](0059-free-threaded-pool-starts-at-two.md) | The worker pool of a free-threaded Python starts at two threads | accepted, amends 0025 |
| [0060](0060-docs-site.md) | A documentation site with one page per concept and a Python/C++ code switch | proposed (seven pages; waiting for the first CI run and the owner's review) |
| [0061](0061-server-gzip-defaults.md) | Server gzip stays off by default, level 1, and its cost is documented | accepted |
| [0062](0062-request-names-match-python.md) | The C++ request names match Python's: `httpp::request` is the client builder, `httpp::server_request` is the handler's request | accepted (rename done and checked on Linux, see Consequences) |
| [0063](0063-benchmark-json-and-docs-page.md) | The benchmark suite writes JSON; the docs site draws the charts | proposed |
| [0064](0064-docs-site-on-github-pages.md) | The docs site is published to GitHub Pages | proposed |
| [0065](0065-file-browser-builder.md) | The file browser is a builder class with enums | accepted, amends 0032 |
| [0066](0066-option-structs-become-builders.md) | Middleware, directory and Swagger UI become builder classes (`cors`, `trusted_host`, `https_redirect`, `directory`, `swagger_ui`) | accepted |
| [0067](0067-urls-are-httpp-url-and-proxy-is-a-url.md) | Every URL parameter is an `httpp::url` (implicit from string); the proxy is one URL with credentials | accepted |
| [0068](0068-socks5-proxy.md) | SOCKS5 proxies (`socks5://`, `socks5h://`) through the same proxy URL | accepted |
| [0069](0069-forwarded-headers.md) | `forwarded_headers`: the real client, scheme and host from a trusted reverse proxy; `server_request::client_ip` | accepted |
| [0070](0070-file-browser-safe-mode.md) | File browser shows html and svg inside a CSP sandbox by default; `set_safe_mode(true)` / `httpp browse --safe` makes them downloads; `directory::set_inline_active` | accepted |
| [0071](0071-client-pool-keeps-every-connection-of-a-burst.md) | The client pool keeps every connection of a burst (idle limit 8 to 256): `httpp.Client` scaling on free-threaded Python | accepted |
| [0072](0072-httpp-url-is-liburlparser-url.md) | `httpp::url` is `urlparser::url` (liburlparser 2.3.0, shared); `url_*` policy functions; `fetch_public_suffix_list` | accepted |
| [0073](0073-file-times-off-hidden-default-and-banner.md) | File times can be switched off (`show_dates`, `--no-dates`), `httpp server` hides dot names (`--hidden`), and the two commands print a small banner on a terminal | accepted |
| [0074](0074-callback-handler-can-run-unserialized.md) | `callback_handler(fn, serialized = false)` calls `fn` without the handler's lock, so a callback that waits for the Python GIL cannot deadlock | accepted |
| [0076](0076-https-for-httpp-server-and-browse.md) | `httpp server` and `httpp browse` serve HTTPS with `--ssl-certfile`, `--ssl-keyfile` and `--ssl-keyfile-password`, the names `httpp run` has | accepted |
| [0077](0077-curl-command.md) | `httpp curl`: a curl-compatible command that only calls `httpp::request` | accepted (merged: 0077, 0083, 0084, 0098) |
| [0078](0078-the-cli-calls-the-library.md) | The `httpp` commands only call the library: `mount` returns the `server&`, and `bind`, `url`, `listen_until_signal`, `set_log_level(name)` and `banner()` are library API | accepted |
| [0079](0079-request-redirects-timeout-compression.md) | `httpp::request`: `max_redirects` is the one redirect setting, plus `connect_timeout` and `auto_compression` | accepted (merged: 0079, 0089) |
| [0081](0081-server-is-a-shared-handle.md) | `server` is a copyable handle that shares its state; `mount` takes and returns it by value; `server_stopper` breaks handler cycles | accepted |
| [0082](0082-swagger-takes-its-openapi-url-and-its-base.md) | `swagger(openapi_url)` replaces `swagger_ui` + `set_docs_path` + `set_openapi_url`; the base is the argument of `mount` | accepted |
| [0086](0086-httpp-utils-base-library.md) | `httpp_utils`: a shared base library (logging, path, sformat, progress) below `httpp_core`; one logger registry per process; two export macros; core file is `libhttpp` | accepted |
| [0087](0087-openapi-document-writer-in-cpp.md) | `httpp::openapi::document` writes OpenAPI JSON in C++ (no parser, no route reading); Python assembles its document through it as `httpp.openapi.Document` | accepted |
| [0088](0088-swagger-header-name-and-mountable-base.md) | `swagger_ui.hpp` becomes `swagger.hpp`; `directory`, `file_browser` and `swagger` derive from `httpp::mountable` (one virtual `mount`) | accepted |
| [0092](0092-settings-names.md) | Setting names: no `set_` prefix, the same names in C++ and Python, keywords as setter names | accepted / implemented (merged: 0092, 0096, 0100, 0103) |
| [0093](0093-client-handles-and-settings-mixin.md) | Client classes: shared handles with `clone()`, and one settings mixin (`configurable`) | implemented, names to confirm (merged: 0093, 0094) |
| [0095](0095-limit-and-unlimited-for-no-limit-settings.md) | a named "no limit" (`httpp::unlimited`) for `max_redirects`, `max_time` and `max_response_size`; the type and the Python spelling are open | proposed |
| [0097](0097-response-reports-elapsed-time-and-bytes-received.md) | `response::elapsed_ms` and `response::bytes_received` come from the library, so `httpp curl` keeps no clock or byte counter of its own | implemented (C++) |
| [0099](0099-retry-in-the-library.md) | `retry`, `retry_delay`, `retry_connrefused` (all three client classes) and `request::on_retry`: sending again after a timeout or a 5xx is the library's job, `httpp curl --retry` only passes the flags | implemented (C++) |
| [0101](0101-download-takes-the-client-settings.md) | download derives from `configurable`: credentials, proxy, TLS, `connect_timeout`, `max_time` as in client and request; move-only; `auto_compression` and `retry*` deleted on it | implemented |
| [0102](0102-save-the-body-to-a-file-from-request.md) | `request::save_to`, `save_in`, `create_dirs`, `overwrite`, `include_head`, `response::saved_as`, `error_kind::file`: writing the answer to a file is the library's job, `httpp curl -o` only passes the flags | implemented (C++) |
| [0104](0104-retry-on-a-download-places-a-206-by-its-content-range.md) | `retry` works on `download`: a 206 is written where its Content-Range says it starts, so a retried or resumed body is never written twice | implemented (C++) |

## Merged ADRs

The numbers below no longer have a file of their own. Look them up here; the text of each is kept as a part of the merged ADR.

| Old number | Is now a part of |
|---|---|
| 0018 | [0011](0011-logging.md) |
| 0050 | [0011](0011-logging.md) |
| 0055 | [0054](0054-path-and-file-system.md) |
| 0056 | [0011](0011-logging.md) |
| 0057 | [0011](0011-logging.md) |
| 0058 | [0054](0054-path-and-file-system.md) |
| 0075 | [0011](0011-logging.md) |
| 0080 | [0011](0011-logging.md) |
| 0083 | [0077](0077-curl-command.md) |
| 0084 | [0077](0077-curl-command.md) |
| 0085 | [0011](0011-logging.md) |
| 0089 | [0079](0079-request-redirects-timeout-compression.md) |
| 0090 | [0011](0011-logging.md) |
| 0091 | [0011](0011-logging.md) |
| 0094 | [0093](0093-client-handles-and-settings-mixin.md) |
| 0096 | [0092](0092-settings-names.md) |
| 0098 | [0077](0077-curl-command.md) |
| 0100 | [0092](0092-settings-names.md) |
| 0103 | [0092](0092-settings-names.md) |

## Not decided yet

These have no ADR because they are not decisions yet. They are tracked in `docs/Architecture.md` section 7 and must be decided **with the project owner**:
the shared memory library and the scope of `app.shared`, the coupling level, the exact API scope (Starlette-like, or FastAPI-like as well),
removing the hop for `async def` handlers that await (see ADR-0023), WebSocket backpressure, the sub-interpreter experiment, a lighter replacement for Swagger UI.

## Writing a new ADR

File name `NNNN-short-english-title.md`; the next free number is the highest number in the table plus one (the numbers of merged ADRs are never reused).
Sections: status, context, decision, rejected alternatives, consequences (including what is **not** verified), related.
Add the row to the table above in number order.
Do not edit an accepted ADR; when a decision changes, write a new ADR and mark the old one "superseded by ADR-NNNN".

## Merging ADRs

When one subject has grown over several ADRs that amend or supersede each other, merge them (the project owner asked for this on 2026-11-20):

- The merged file keeps the lowest number of its parts and gets a name for the subject. It starts with a "Current state" summary, then holds every part unchanged
  (own heading `### ADR-NNNN: title`, its headings moved two levels down), in number order.
- The other numbers are added to the "Merged ADRs" table and their files are deleted. Links to the old file names are changed to the merged file; the plain
  text "ADR-NNNN" elsewhere is left alone and stays valid through the table.
- Never renumber and never reuse a number.
