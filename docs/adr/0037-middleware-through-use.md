# ADR-0037: Middleware through `server::use()` and ready-made hooks

> Note (ADR-0066): the options structs and `add_*` functions below are now the classes `cors`, `trusted_host` and `https_redirect` with `install(server&)`.

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

5. Added later: `basic_auth` (Python `BasicAuthMiddleware(users, realm, path_prefix)`) and `rate_limit` (Python `RateLimitMiddleware(requests_per_second, burst, max_clients)`), both builders with `install(server&)` like the others, requested by the project owner.
   - `basic_auth` checks `Authorization: Basic` (RFC 7617: strict base64, the first `:` splits the decoded text), compares every configured entry in constant time, and answers 401 with `WWW-Authenticate: Basic realm="...", charset="UTF-8"`. Users are entries `"user:password"` in memory (a gate for a small tool, not a user database; pointless without TLS). `set_path_prefix` limits it to the paths under a prefix. It is a plain before hook, so `cors` must be installed first or a preflight, which carries no credentials, is refused.
   - `rate_limit` is a token bucket per `server_request::client_ip` (so `forwarded_headers` goes first behind a proxy), answers 429 with `Retry-After`, and bounds its table (`max_clients`: idle clients are dropped first, then an arbitrary one). The state lives in the hook, one table per `install`. Single process only: several worker processes each count alone.
   - Cookies needed nothing new: `server_request::cookie`, `response::set_cookie` and `httpp.set_cookie` already exist. There is no `request.basic_auth()` helper; a handler that needs the user name can read the header itself.
   - Not done: hashed passwords or a verifier callback (a Python callback would take the GIL per request, against the point of these classes), per-route limits, `RateLimit-*` headers, a shared limit across processes.

## Rejected alternatives

- Replace `before` and `after` by a list: a breaking change for no gain; `use()` keeps both APIs.
- Write the middleware in Python on top of `before_request`: costs a GIL round trip per request and cannot answer a preflight on the I/O thread.
- Make `https_redirect` part of this step: the server has no TLS and no notion of a request scheme yet, so a redirect rule would loop. It came with ADR-0040 (server TLS), which added `request::secure` and `add_https_redirect`.

## Consequences

- Cost with no middleware: none (one empty `std::function` check, as before). With middleware: one extra `std::function` call per layer and per hook; no allocation except the CORS headers.
- `ponytail:` the www redirect re-encodes the decoded path and query because `request` has no raw target. Upgrade path: expose the raw target.
- `ponytail:` the scheme of that redirect is always `http` until the server has TLS.
- Verified: 16 new C++ tests (`tests/cpp/test_server_middleware.cpp`), the whole C++ suite (423 tests, Release), 6 new Python tests, the whole Python suite on 3.13 (671 passed, 4 skipped).
  Not run: sanitizers on this change, Python 3.14 and 3.14t, Windows, macOS, a browser against the CORS answers, throughput with middleware.
