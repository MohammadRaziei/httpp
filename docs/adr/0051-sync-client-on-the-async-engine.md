# ADR-0051: The blocking client runs on the async engine and one shared policy layer

- **Status:** accepted (step 1 of retiring cpp-httplib; supersedes the "sync client on cpp-httplib" row of ADR-0003 for `httpp::client`)
- **Decided by:** the project owner (retire cpp-httplib; copy from it where useful; no duplicated code, modular OOP)
- **Source:** `src/core/internal/http_exchange.hpp`, `src/core/http_exchange.cpp`, `src/core/client.cpp`, `tests/cpp/test_client_blocking_engine.cpp`

## Context

`client`, `client::request::run` and `client::fetch` ran on cpp-httplib, one blocking thread per request, while `request::run_async` and `async_client` ran on our engine with
their own copy of redirects, auth, cookies, proxy and TLS policy. The settings (headers, timeout, redirects, auth, proxy, verify, CA file) were kept three times, in three shapes.

## Decision

One policy module, `internal/http_exchange`, owns everything about *what* to send; the engine (`async_http`) only moves bytes.

| Piece | Role (pattern) |
|---|---|
| `session_settings` | what a caller configures once; `make_spec(...)` builds one request from it (Builder). Used by `client`, `client::request` and `async_client`. |
| `request_spec` | one logical request, a plain value; a redirect derives the next hop from it |
| `run_chain` | runs a spec on the engine and follows redirects (the policy loop, moved out of `client.cpp` unchanged) |
| `run_blocking` | waits for `run_chain` (Adapter: sync on top of async) |

`client.cpp` is now only base-URL handling, the fluent builders and the sync/async calling styles. Nothing in it names httplib. Behaviour changes, all deliberate:

- A blocking call made on the engine's own thread (inside a `run_async` callback) returns `error_kind::other` at once instead of deadlocking (`async_http::on_loop_thread`).
- A `client` is no longer serialized by a mutex: threads may share one and calls run side by side. Only reading and writing the settings take a lock; a request keeps the settings it started with.
- Connections are pooled in the engine by (host, port, TLS, proxy), so two `client` objects to the same server share them. `client::fetch` and `set_keep_alive(false)` do not park the connection.
- `async_client::set_header` replaces a same-name default (it used to append a duplicate), as `client::set_header` always did. A per-call header still wins over a default.
- If both Basic and Bearer credentials are set, Basic is sent (the async path already did this; the old blocking path sent whichever httplib applied).

## Rejected alternatives

Keeping a dedicated connection per `client` (needs a session id in the pool key and gives up sharing for no gain); a second adapter class around `async_client` (it adds a queue the blocking
path does not need); copying httplib's client code (it is the blocking design we are leaving, and the policy we need already exists and has tests).

## Consequences

- Downloads and `curl.cpp` still use cpp-httplib (steps 2 and 3). `httplib_common.hpp` and the macOS Keychain link flags stay until then.
- One place for redirect, TLS, proxy and auth bugs; no thread is blocked inside a socket call.
- Not verified: Windows, macOS, Python 3.14t, the Python test suite (Cython was not available where this step was made), a comparison against live public sites.

## Related

ADR-0003, ADR-0004
