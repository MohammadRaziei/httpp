# Changelog

All notable changes to httpp are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project follows
[Semantic Versioning](https://semver.org/) (while the major version is 0, a minor release may still change an API;
such changes are listed under **Changed**).

## [Unreleased]

### Added
- **`server::set_body_timeout(seconds)` and `Server(body_timeout=...)`**: the whole request body must arrive within this time after
  its head (default 120 s, `0` = no limit). Before, a body dripped one byte at a time was held only by the per-read `read_timeout`.
- **`server::set_compression(min_size, level = 1)` and `Server(compression=..., compression_level=...)`**: gzip for text answers
  (`text/*`, JSON, XML, JavaScript, SVG) when the client sends `Accept-Encoding: gzip`. Off by default. It runs after the hooks, adds
  `Vary: Accept-Encoding`, and leaves files, streams, partial answers, already encoded bodies and bodies it cannot shrink alone. On a
  `route_io` answer the work moves to a worker, never the I/O thread. Uses the vendored deflate: no new dependency.
- **Routers**: `httpp::router` and `server::include(router, prefix)` in C++, `APIRouter` and `Server.include` (alias `include_router`) in
  Python. Same rules in both: outer prefix, the router's own prefix, then the path; `/` under a prefix is the prefix. Nesting works.
  No request-time cost: prefixes are joined when the router is included.
- **`response::after_send`** (C++) and, in Python, the injected **`response`** argument and **`BackgroundTasks`**: a job that runs on a
  worker thread after the answer was written, for work the client should not wait for. The injected `response` adds a status,
  headers and cookies to a handler's answer. A throwing job is logged and ignored.
- **Forms and uploads**: `request::form()` in C++ (`form_data`, `upload_file`) parses urlencoded and `multipart/form-data` bodies, only when a
  handler asks. Python: `request.form()` (a malformed body answers 400), and FastAPI-style `Form()`, `File()` and `UploadFile` parameters
  with 422 errors and OpenAPI request bodies. `percent_decode` and `parse_query` moved to one internal header.
- `ServerRequest.header()`, `.query_param()`, `.path_param()` and `.cookie()`: the lookups the C++ `request` already had, same names,
  `""` when absent (a second argument sets another default).
- ADR-0022: the dual C++/Python rule and the decisions the owner delegated.

### Changed
- A failing `done()` of an `async def` route (for example a bad header) now answers a 500 at once instead of leaving the client to the 504.
- **The worker pool wakes threads last-in first-out, and does not wake a second thread until a job has waited 500 us** (a scout
  thread watches the queue), so light `def` handlers run on one hot thread instead of passing the Python lock around. On two cores,
  `def` `hello` went from 16k to 36k req/s (3.13) and from 19k to 33k (3.14t); `item` from 12k to 25k and from 17k to 30k. A
  blocked handler delays queued requests by about 500 us. New `tests/cpp/test_worker_pool.cpp` (also run under ThreadSanitizer).

### Changed (async)
- **`async def` routes are about 30 percent faster:** one shared timer for `handler_timeout` instead of a timer per request, and a prebuilt JSON
  encoder (same output as `json.dumps`; CPython only, a circular structure now ends in a RecursionError, still a 500).
- **`async def` handlers start eagerly on Python 3.12+:** a handler that never waits finishes inside `dispatch` without a trip through the
  loop's ready queue (2 to 8 percent on `hello` / `item`).
- **The `async def` inbox is drained in batches:** one lock per batch of queued requests instead of one per request (about 12 percent on
  `hello`, 17 percent on `item`, 2 cores).
- **Faster answers for `str`, `dict` and `list` results,** and no `handler_timeout` bookkeeping for a handler that finishes at once
  (together with the eager start about 20 percent on `hello` / `item`, 2 cores).

### Fixed
- **`Server.listen()` could hang for ever after Ctrl+C on free-threaded Python (seen on the macOS 3.15t wheel test):** it waited on a
  `threading.Event`, and an interrupt that lands inside `Event.wait()` can leave that Event's internal lock taken (or released twice), after which
  `listen()` and the serving thread both block on it. It now polls a plain flag every 50 ms (`time.sleep` holds no lock). Two tests: Ctrl+C at
  many different moments, and a deterministic one that `listen()` never creates an `Event` or `Condition`.
- **The macOS wheel build works again:** `serve_directory` checked symlinks with `std::filesystem`, which needs macOS 10.15 while the
  wheels target 10.13. It now uses `realpath` on Linux and macOS (`std::filesystem` stays on Windows only). The tests also cover a sibling
  directory whose name starts like the mounted one, and a mounted directory given with a trailing slash.
- **`def` handlers were slow: a Python thread state was created and destroyed for every request.** Each pool thread now keeps
  its state (`httpp_py_pin_thread`). Measured on 2 cores, `oha`, 32 connections, plain `hello` route: CPython 3.13 9.4k -> 17.9k
  req/s, free-threaded 3.14.6t 1.9k -> 24.6k req/s. A CPU-bound `def` handler on 3.14t now scales with the cores
  (conc 1: 382, conc 8: 734 req/s) while 3.13 stays at about 330. A client that hangs up now closes a sync streaming generator
  at once (its `finally` runs), because on free-threaded CPython an object released from another thread is freed only when
  the thread that created it next runs Python.
- **A dependency without parameters failed on a route with path parameters** (`TypeError: dep() got an unexpected keyword argument 'n'`):
  every dependency was handed all the path parameters of the route, not only the ones it declares. Found by a stress test; it
  happened on every Python version.
- **405 instead of 404 for a wrong method.** A path that has a route for another method answers 405 with an `Allow` header
  (a GET route also allows HEAD). A path with no route at all is still 404. (Changed: code that checked for 404 on a wrong method.)
- **Static files no longer follow a symlink out of the mounted directory.** Such a link answers 404; a link that stays inside is served.
- **A path parameter that does not parse** (`n: int`, `x: float`) answers 422 with the FastAPI body
  `{"detail": [{"type": "int_parsing", "loc": ["path", "n"], ...}]}` instead of the old hand-made `{"error": ...}`.
- **abi3 wheel build.** `sha256_hex` took a typed memoryview, which needs `Py_buffer`; the stable ABI has it only from Python 3.11,
  and the cp39-abi3 wheel failed to compile (CI only). It now takes any bytes-like object without one (`bytes` in place, others copied
  once). `tests/python/test_abi3_safe.py` guards against typed memoryviews coming back.

### Added
- **Slowloris guard and a connection limit.** The whole request head (request line and headers) must now arrive within
  `header_timeout` seconds (default 10, `0` = no limit); before, a client sending one byte every few seconds kept the connection
  forever because only each single read had a timeout. `set_max_connections(n)` (C++) and `Server(max_connections=n)` refuse
  the connection over the limit with a 503 and close it (default: no limit). A slowly dripped request **body** is still only
  covered by `read_timeout` per read.
- **`HTTPException`, response classes, `httpp.status`.** `raise httpp.HTTPException(404)` (or `(status, detail, headers)`) in a
  handler or a dependency answers `{"detail": ...}` with that status; override it with `@app.exception_handler(HTTPException)`.
  `JSONResponse`, `PlainTextResponse`, `HTMLResponse`, `RedirectResponse` (307 by default), `StreamingResponse` and `FileResponse`
  can be returned from a handler and skip `response_model`. `httpp.status.HTTP_404_NOT_FOUND` and the rest. See ADR-0020.
- **gzip / deflate in the client, no system zlib.** Both the blocking and the async client send `Accept-Encoding: gzip, deflate`
  (unless you set the header yourself) and undo `Content-Encoding: gzip`, `x-gzip`, `deflate` (zlib, or raw as some servers send it)
  and chains of them, so `response.body` is always the real body. Headers are left as received, like requests and httpx. A
  coding we do not advertise (`br`, `zstd`) leaves the body untouched. `max_response_size` limits the **decoded** size
  (a zip bomb ends as `too_large`, status 413); corrupt data ends as `error_kind::other`, never as garbage. Set
  `Accept-Encoding: identity` to ask the server not to compress. `download()` is unchanged in what it sends but now saves a compressed response exactly as received, instead of failing with 415. Built on two vendored decoders/encoders, extracted unmodified:
  `src/third_party/stb_inflate` (the decoder of stb_image.h) and `src/third_party/stb_deflate` (the compressor of libstb), wrapped
  by the internal `httpp::detail::inflate` / `deflate` (zlib, gzip and raw; levels 0 to 9; Adler-32 and CRC-32 checked), and tested
  against Python's zlib and gzip in both directions. The compressor is ready for the server side (a GZip middleware) and for
  compressing request bodies; neither is wired yet.
- **Access log.** At the `info` log level the server writes one line per answered request (`GET /items/1 200`),
  including the answers the server makes itself (404, 413, ...). It goes through the library's own logging
  (AixLog), so `set_log_level`, `set_log_file` and `set_log_callback` apply to it. Nothing is built or written
  while the level is higher.
- **"Listening" line.** After a successful bind the server logs `listening on http://host:port` at `info`
  (also for `bind_to_any_port`, with the real port). A port that is already taken only produces the existing
  `cannot listen` error.

- **Python parity for downloads and hashing.** `Download.sha256()`, `Download.sha256_url()`, `Download.set_timeout()`,
  `Download.set_max_wait(seconds)` and `DownloadResult.sha256`; `httpp.utils.sha256_hex()`, `httpp.utils.sha256_file()` and
  `httpp.utils.parse_sha256_checksum()` (they raise `RuntimeError` where the C++ functions throw). `Client(keep_alive=False)`
  mirrors `httpp::client::set_keep_alive`.
- **Parity test.** `tests/python/test_cpp_python_parity.py` fails when a public C++ name has no Python counterpart and no documented reason.

### Changed
- **The C++ log goes through Python's `logging` automatically.** Lines arrive in `logging.getLogger("httpp")`, and that logger's level
  decides what C++ produces: `logging.getLogger("httpp").setLevel(logging.INFO)` (or `logging.config.dictConfig`) now also turns on the access
  log and the `listening on ...` line. `httpp.set_log_level(...)` sets the same logger level. `httpp run` still prints to stderr when the
  application configured no logging. An explicit `set_log_callback`/`set_log_file` replaces the bridge. See ADR-0018.
- `Client`/`AsyncClient` encode query strings and forms with the C++ `url::build_query` instead of `urllib` (identical output, about 4x
  faster for that step). One path-template parser in Python instead of two; one shared option block for `Client` and `AsyncClient`.
- **`httpp.run`, `app.run()` and `httpp run` log at `info` by default** (startup line and one line per request on
  stderr, like uvicorn). Use `log_level="warning"` or `--log-level warning` to keep only problems, `off` for
  nothing. `app.listen()` and the C++ API keep the default `warning`, so an embedded server stays quiet until you
  call `set_log_level`. Benchmarks should run at `warning`.
- **llhttp moved to `src/third_party/llhttp`** (it was `src/llhttp`). The files are unmodified: they match the checksums in
  `VENDORED.md` and the upstream `release/v9.4.3` tag byte for byte. Only the path changed.
- The `starting on ...` line printed by `run` before binding is gone; the `listening on ...` line replaces it and
  only appears once the port is really bound.
- **`httpp run` wins over a `listen` in the file.** While `httpp run module:app` imports the module, `app.listen()`
  and `app.run()` calls at import time (without an `if __name__ == "__main__":` guard) are skipped with a warning
  (`httpp: ignoring app.listen('127.0.0.1', 8001): ...`), so `--host`, `--port` and `--workers` apply. Before, the
  file's `listen` blocked the import, the CLI options were silently ignored, and `--workers` failed with a bind
  error. Running the file with `python file.py` is unchanged. Only a string target (`"module:app"`) is affected.

## [0.7.0] - 2026-10-17

The biggest release so far: a native async client and server, a FastAPI-style Python layer with OpenAPI and Swagger
UI, and a download that verifies, locks and times out.

### Added

#### Client
- **Async client** (`Client.run_async()`, `AsyncClient`) running on the library's own non-blocking network thread
  (Asio) with the vendored `llhttp` parser, instead of a shared thread pool. Keep-alive connection pooling and reuse
  across requests, redirect handling, TLS and proxy support.
- **Streaming responses** and **structured errors** (`httpp.errors`, with an error kind such as `timeout`) for both
  the blocking and the async client; extra URL helpers in `httpp/url.hpp`.

#### Server (C++ and Python)
- **Async HTTP server** (`async_server`, with a worker pool). It speaks plain HTTP: the server has no TLS yet. TLS (Mbed TLS)
  is in the async client, with session caching: TLS contexts for identical (verify, CA file) settings are built once and reused.
- **Python handlers:** route handlers with path parameters, `async def` handlers, and `listen()` that blocks and
  returns a status (0.6.1); new in this release: `ResponseHead`, `file()`, `redirect()`, `set_cookie()`,
  `ServerRequest.cookies`, streaming responses from generators and async generators, `@app.before_request`,
  `@app.after_request`, error and exception handlers, startup and shutdown hooks, and `handler_timeout` (60 s by
  default) that cancels a hanging async route and answers 504. Header values are checked against injection.
- **OpenAPI 3.1 and Swagger UI for Python.** Schema generation with Pydantic v2 models, per-route options (`summary`,
  `tags`, `response_model`, ...), and `Query`, `Path`, `Header`, `Cookie`, `Body` and `Depends` (dependencies with their
  own scopes and caching) that drive both the request handling and the generated document, so the docs cannot drift
  from the behaviour.
- **`httpp::mount_swagger_ui(server, docs_path = "/docs", openapi_url = "/openapi.json", title = "API")`** in C++:
  serves the page and the Swagger UI files (`swagger-ui-bundle.js`, `swagger-ui.css`, `swagger-api-icon.svg`) from
  bytes compiled into the library, with `Cache-Control: public, max-age=86400`. Title and URL are escaped; an invalid
  path throws `std::invalid_argument`. The Python server's `/docs` is now served from C++, without the GIL, and there
  is no CDN or data directory involved.
- **`cmake/EmbedFile.cmake`** (`embed_files()`): compiles data files into a header at configure time (`constexpr`
  data in `.rodata`, binary-safe, 32 bytes per source line).

#### Downloads
- **`download::sha256(hex)` and `download::sha256_url(url = "")`**: verify the finished file against a SHA-256 digest
  (any letter case) or against a checksum file in `sha256sum` format (default `<url>.sha256`). The checksum file is
  fetched first, so a missing or malformed one (an HTML error page, a name for another file, several lines) fails the
  call before any download. A mismatch deletes the file, returns `ok = false` and says what the digest actually was.
  With `resume()`, a complete-looking but corrupt file is downloaded again once from byte 0. `download_result` has a
  new `sha256` field.
- **`httpp/sha256.hpp`**: `sha256_hex()`, `sha256_file()` (chunked) and `parse_sha256_checksum()`, on top of the
  Mbed TLS already linked for HTTPS. About 165 MB/s on the machine it was measured on (130 MB file in 0.8 s).
- **`download::set_timeout(seconds)`**: connect, read and write timeouts of the transfer (like
  `Client::set_timeout`), and also how long to wait for another process that holds the same file's lock.
- **`download::set_max_wait(milliseconds)`**: how long a lock that is *not being updated* is waited for before its
  owner is taken for dead (default 2 s; the wait is `min(max_wait, timeout)`).
- **Cross-process lock, always on.** A download takes the directory `<dest>.lock` and its owner rewrites
  `<dest>.lock/alive` every 200 ms at most; a lock that keeps being updated is waited for as long as it lives, one that
  stops is taken over after `min(max_wait, timeout)`. A waiter that finds the file finished and matching `sha256()` /
  `sha256_url()` uses it without a request (`skipped = true`). If the lock directory cannot be created the download
  goes ahead unlocked. Written with `mkdir`/`rmdir` and plain files, no `<filesystem>`.

#### Library and tooling
- **Logging** through the vendored AixLog, with a `set_log_*` API in C, C++ and Python.
- **Python wheels:** one `cp39-abi3` wheel per platform for every GIL-enabled Python from 3.9 up, plus version-specific
  free-threaded wheels (`cp313t`, `cp314t`, `cp315t`), because free-threaded builds cannot load abi3 wheels.
- **Documentation:** `docs/Architecture.md` (design, goals and test strategy), `docs/agent.md`, Python micro-benchmark
  for the validators, and README sections for streaming, files, hooks, error handling and the client's new
  capabilities.

### Changed
- **Mbed TLS 4.2.0** (release tag), with threading support switched on (`MBEDTLS_THREADING_C`, via
  `cmake/httpp_mbedtls_config.h`): pthread on POSIX, a small `SRWLOCK`/`CONDITION_VARIABLE` layer on Windows, or on
  every platform with `-DHTTPP_MBEDTLS_THREADING_ALT=ON`. `MBEDTLS_SELF_TEST` is off.
- **`HTTPP_EMBED_AS_ARRAY` is `ON` on every platform** (it was MSVC only). One code path everywhere and no
  `-Woverlength-strings` warning for the 1.5 MB Swagger UI bundle. Cost: compiling that translation unit takes about
  4.6 s instead of 0.9 s with GCC, once per build. `-DHTTPP_EMBED_AS_ARRAY=OFF` gives string literals again.
- The Swagger UI tab icon is a real SVG file served at `/docs/swagger-api-icon.svg`; the PNG favicon is gone.
- `<filesystem>` is not used any more (macOS 10.13 compatibility); `download.cpp` and the lock use `std::ifstream`,
  `std::remove` and platform calls.
- Asio now drives both server loops and the async client (documentation and build comments updated).
- Build requirement: `scikit-build-core[pyproject]>=1.1`; the version is read through the standard
  `[[tool.dynamic-metadata]]` table instead of the deprecated `tool.scikit-build.metadata`.
- A download creates (and removes again) `<dest>.lock` next to the destination file; the destination's directory must
  be writable for the lock to work (otherwise the download is not locked).

### Fixed
- Concurrent TLS handshakes shared Mbed TLS's global random generator without a lock; two handshakes could draw the
  same random bytes. Found with ThreadSanitizer, fixed by the threading support above.
- Async `after`/`error` hooks, or a slow client, could block the event loop and deadlock concurrent requests; the
  Python adapter now dispatches synchronous work directly when no async hook is registered.
- **Windows:** a finished download did not replace an existing file (`std::rename` fails when the target exists); it is
  now an atomic `MoveFileExA(MOVEFILE_REPLACE_EXISTING)`. `ws2_32` is linked for `send()`/`WSAGetLastError()`, and the
  `EmbedFile` generator no longer emits a zero-size array for an empty file (MSVC `C2466`).
- **macOS:** link the Keychain frameworks for the C++ tests; a URL-encoding test relied on `std::isalnum` being false
  for non-ASCII bytes.
- Coverage builds use atomic profile counters (the server is multi-threaded, lcov saw negative counts); free-threaded
  wheel builds prefer binary wheels so `rpds-py` is not built from source.
- Tests for concurrent async downloads measure event-loop ticks and overlapping streams instead of wall-clock time.
- **Release pipeline:** wheels are uploaded as one flat artifact, so `dist/` holds only `.whl` and `.tar.gz` files and
  the PyPI upload no longer fails on a `wheelhouse` sub-directory. Windows wheels are not run through `delvewheel`
  (`httpp_core.dll` is loaded from `httpp/lib/` at import time), and the PyPI step uses `skip-existing`.

### Known limitations
- The async client has no timeout yet, and async hooks hold a thread while they run.
- `download::set_timeout`, `set_max_wait`, `sha256` and `sha256_url` are not exposed in the Python binding yet.
- A lock whose owner stops updating it for more than `max_wait` (heavy load, a suspended process) is taken over, and two
  downloads can then run at once. Two waiters taking over the same dead lock in the same instant, or writing the same
  destination, can write it together; use `sha256()` so a damaged result is caught and deleted. Keep `max_wait`
  at 1 s or more.
- The Windows and macOS changes of this release were checked by CI only; the lock was not run under ThreadSanitizer.
- `/docs` has been looked at in Chromium only.

## [0.6.1] - 2026-10-03

### Added
- Python route handlers with path parameters and `async def` handlers; native async support for Python and C++
  handlers.
- `listen()` blocks and returns a status in the C++ and Python bindings.
- Runnable standalone Python examples: server basics, routing, request and response handling, async handlers, static
  files, error handling.

### Changed
- Asio 1.34.2 is a git submodule (no system dependency, better scalability).
- README rewritten with a clearer description and structure; the Asio documentation was consolidated.

### Fixed
- Windows: winsock type redefinition and missing socket headers in the tests; a Ctrl+C listener test that cannot run
  there (Popen) is skipped and the SIGINT test was replaced with an `interrupt_main` variant.

## [0.6.0] - 2026-09-16

### Added
- Resumable downloads with skip-if-complete.

### Changed
- **Breaking:** `download_file` was renamed to `download` and folded into one fluent builder.

## [0.5.1] - 2026-09-16

### Added
- `max_response_size` for the client and `follow_redirects` for downloads.
- `abi3` wheels in CI.

### Changed
- `std::filesystem::remove` replaced by `std::remove` in downloads.

## [0.5.0] - 2026-09-15

### Added
- HTTPS support through Mbed TLS (documented in the API docs).

### Changed
- Mbed TLS sources are generated before CMake configures; `jinja2` is a build dependency; submodules are no longer
  fetched recursively by default.
- macOS: the Keychain frameworks are linked into `httpp_core`.
- Vendored-code warnings are suppressed and class-level exports avoided.
- Linux wheels are repaired with `auditwheel`.

## [0.4.0] - 2026-09-14

### Fixed
- Linux wheels are repaired with `auditwheel` in cibuildwheel builds.

## [0.3.0] - 2026-09-14

Release only; no functional change recorded in the history.

## [0.2.0] - 2026-09-13

### Added
- curl-compatible API and CLI commands.

## [0.1.1] - 2026-09-13

### Fixed
- Linux wheels are repaired with `auditwheel` instead of copying libraries.

## [0.1.0] - 2026-09-12

First release.

### Added
- C++17 core: client, server and URL parsing (`liburlparser`, IPv4, IPv6 and Public Suffix List aware).
- Downloads with a progress bar (vendored `indicators`).
- Python package with `httpp_core` as a shared library bundled in the wheel; macOS rpath handling.
- CMake package: consumers find httpp through `httpp.get_cmake_dir()`; `HTTPP_BUILDING_DLL` for the shared core.
- Single umbrella header `include/httpp.h` that carries the version, and `version.py` for tagging and releases.
- CMake and wheels workflows in CI.

[0.7.0]: https://github.com/MohammadRaziei/httpp/compare/v0.6.1...v0.7.0
[0.6.1]: https://github.com/MohammadRaziei/httpp/compare/v0.6.0...v0.6.1
[0.6.0]: https://github.com/MohammadRaziei/httpp/compare/v0.5.1...v0.6.0
[0.5.1]: https://github.com/MohammadRaziei/httpp/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/MohammadRaziei/httpp/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/MohammadRaziei/httpp/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/MohammadRaziei/httpp/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/MohammadRaziei/httpp/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/MohammadRaziei/httpp/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/MohammadRaziei/httpp/releases/tag/v0.1.0