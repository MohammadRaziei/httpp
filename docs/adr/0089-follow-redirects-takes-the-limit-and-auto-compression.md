# ADR-0089: `request::max_redirects(n)` is the one redirect setting (0 = none); `auto_compression` on `httpp::request`

- **Status:** accepted
- **Decided by:** the project owner ("two concepts, `follow_redirects` and `max_redirects`, is not wanted"; "for zero it means do not follow"; "an `auto_compression(true)` is easy to add")
- **Source:** `include/httpp/client.hpp`, `src/core/client.cpp`, `src/core/http_exchange.cpp`, `src/core/internal/http_exchange.hpp`, `src/core/internal/async_http.hpp`, `src/core/async_http.cpp`, `src/core/url.cpp`, `src/bindings/python/cy/{declarations,transfer}.pxi`, `src/bindings/python/httpp/_http.py`, `src/cli/httpp_cli.cpp`
- **Follows:** ADR-0079, ADR-0084

## Context

- `httpp::request` had `follow_redirects(bool)` and (ADR-0079) `max_redirects(n)`: two settings for one idea. A limit without `follow_redirects()` did nothing, and `follow_redirects().max_redirects(0)` meant "fail on the first redirect", which is a third behaviour nobody asked for.
- The client always sent `Accept-Encoding: gzip, deflate` and always decoded the body; there was no way to get the coded bytes.

## Decision

- `request::max_redirects(int count)` is the only redirect setting of a request. `0`, the default, follows none: a 3xx is the answer. `n > 0` follows up to `n` hops; one more fails with `too_many_redirects`. `-1` follows without a limit and without an error, as in curl. Any other negative number throws `std::invalid_argument` (Python: `ValueError`).
- `request::follow_redirects` is removed (C++ and `Request` in Python). `client::set_follow_redirects`, `async_client::set_follow_redirects` and `download::follow_redirects` stay: they are session or transfer settings and are not part of this change.
- The Python helpers (`httpp.request`, `get`, `post`, ...) take `max_redirects=N`, and still accept `follow_redirects=True` as in `Client(...)`, which means `max_redirects=20`.
- `httpp curl -L` means `max_redirects(20)`, `-L --max-redirs N` means `max_redirects(N)` (and `-1` means no limit). `-L --max-redirs 0` keeps curl's behaviour (exit code 47 on the first redirect): the command checks the first head itself.
- `request::auto_compression(bool enable = true)` (Python: `Request.auto_compression`, `httpp.request(..., auto_compression=False)`). On, the default, is the old behaviour. Off: no `Accept-Encoding` is sent unless `header()` sets one, and a gzip / deflate body comes back exactly as the server sent it.
- `client` and `async_client` do not get these settings yet; the fields live in the shared `session_settings`.

## Consequences

- With `-1` a redirect loop never ends, because `timeout` limits each step and not the whole request. A deadline for the whole request would close this; it is not part of this ADR.
- `follow_redirects()` on `request` is a breaking removal; the version is still 0.x and it is listed under **Changed**.
- `httpp curl` keeps `--compressed` as an accepted no-op: it streams the answer, so the request already asks for `Accept-Encoding: identity`. Making it real needs decoding in the streaming path, which is separate work.

## Related

ADR-0079, ADR-0084
