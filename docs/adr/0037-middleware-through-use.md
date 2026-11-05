# ADR-0037: Middleware through `server::use()` and ready-made hooks

- **Status:** accepted
- **Source:** `include/httpp/middleware.hpp`, `src/core/middleware.cpp`, `server::use` in `src/core/server.cpp`, `src/bindings/python/httpp/middleware.py`

## Context

The server had one `before` and one `after` hook, and the Python layer chained any number of user hooks behind them. A framework that replaces FastAPI/Starlette needs the usual middleware
(CORS, trusted hosts, gzip) and they must not need the GIL or a Python call per request. The owner delegated the design (2026-11-06).

## Decision

1. `server::use(before_hook, after_hook = {})` adds a middleware layer. The engine still calls one composed hook of each kind (or none, so the "no hooks" fast path stays), rebuilt
   whenever `use`, `before` or `after` is called. Order: middleware before hooks in registration order, then the application's `before`; the first that answers ends the chain.
   After hooks: the application's `after`, then the middleware in reverse order (an onion). `before()` and `after()` replace only the application's own hook, never middleware.
2. Ready-made middleware are free functions over `server&`, like `mount_file_browser`: `add_cors(cors_options)` and `add_trusted_host(trusted_host_options)`. Options and behaviour follow
   Starlette (preflight answered with 200 or a 400 reason, `Vary: Origin`, `*` with credentials echoes the origin, regex must match the whole origin, `*.example.com` matches subdomains only).
3. GZip is not a new engine feature: `GZipMiddleware(minimum_size, compresslevel)` calls the existing `set_compression` (default level 1, not Starlette's 9, for speed).
4. Python: `Server.add_middleware(CORSMiddleware, **options)` with the same option names; the classes validate their options and install the C++ middleware. Arbitrary ASGI middleware is not
   supported (there is no ASGI layer). The C++ `use()` has no Python counterpart on purpose: Python code that wants its own hooks has `before_request` and `after_request`.

## Rejected alternatives

- Replace `before` and `after` by a list: a breaking change for no gain; `use()` keeps both APIs.
- Write the middleware in Python on top of `before_request`: costs a GIL round trip per request and cannot answer a preflight on the I/O thread.
- Make `https_redirect` part of this step: the server has no TLS and no notion of a request scheme yet, so a redirect rule would loop. It comes with ADR for server TLS.

## Consequences

- Cost with no middleware: none (one empty `std::function` check, as before). With middleware: one extra `std::function` call per layer and per hook; no allocation except the CORS headers.
- `ponytail:` the www redirect re-encodes the decoded path and query because `request` has no raw target. Upgrade path: expose the raw target.
- `ponytail:` the scheme of that redirect is always `http` until the server has TLS.
- Verified: 16 new C++ tests (`tests/cpp/test_server_middleware.cpp`), the whole C++ suite (423 tests, Release), 6 new Python tests, the whole Python suite on 3.13 (671 passed, 4 skipped).
  Not run: sanitizers on this change, Python 3.14 and 3.14t, Windows, macOS, a browser against the CORS answers, throughput with middleware.
