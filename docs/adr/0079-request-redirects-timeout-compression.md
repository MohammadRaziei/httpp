# ADR-0079: `httpp::request`: `max_redirects` is the one redirect setting, plus `connect_timeout` and `auto_compression`

- **Status:** accepted (merged ADR; each part keeps its own status line)
- **Merged on:** 2026-11-21, at the request of the project owner: the decisions of one subject were spread over 2 ADRs. The text of every part is kept, as a section below, in the order it was decided.

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history. Where a part and this summary disagree, this summary wins.

- **`request::max_redirects(n)` is the only redirect setting.** `0` (the default) follows none and a 3xx is the answer; `n > 0` follows up to `n` hops and one more fails with `too_many_redirects`; `-1` follows without a limit; any other negative number throws `std::invalid_argument`. `request::follow_redirects` is removed; the first part describes the earlier shape where it still existed.
- **`request::connect_timeout(seconds)`** limits the connect step. `timeout` and `-m` still limit each step, not the whole transfer.
- **`request::auto_compression(bool)`** (default on) switches `Accept-Encoding` and decoding off when false.
- There is no `user_agent()` or `referer()` on purpose: `header()` covers them.
- Later naming and getter changes of these settings are in ADR-0092.

## Parts
### `httpp::request` gets `max_redirects` and `connect_timeout`

- **Status:** accepted; the part "`request::max_redirects(n)` is the one redirect setting (0 = none); `auto_compression` on `httpp::request`" made `max_redirects` the only redirect setting (0 follows none, `request::follow_redirects` is removed)
- **Decided by:** the project owner ("enrich `httpp::request`, not a second class", then "agreed" to the first candidates)
- **Source:** `include/httpp/client.hpp`, `src/core/client.cpp`, `src/core/http_exchange.cpp`, `src/core/internal/http_exchange.hpp`, `src/core/internal/async_http.hpp`, `src/core/async_http.cpp`, `src/bindings/python/cy/{declarations,transfer}.pxi`, `src/bindings/python/httpp/_http.py`, `src/cli/httpp_cli.cpp`
- **Follows:** ADR-0077

#### Context

ADR-0077 settled that curl-like needs are met by enriching `httpp::request`, not by a second API. Looking at what `httpp curl` still lacked against what the engine could do:

- The redirect limit was a constant (`request_spec::redirect_limit`, 20); no caller could change it.
- `timeout` is one number for every step the engine takes (resolve, connect, write, each read). There was no way to say "fail fast when the host is unreachable, but let a slow answer take its time".
- `user_agent` and `referer` were on the first candidate list, but both are plain headers: `request::header("User-Agent", ...)` already sets them, and the engine only adds its default `User-Agent: httpp` when none is given. A method of their own would be a second way to do the same thing in C++ and again in Python.

#### Decision

- `request::max_redirects(int count)`: how many redirects `follow_redirects()` may follow before the request fails with `too_many_redirects`. Default 20. `0` follows none (the first redirect fails). Without `follow_redirects()` it changes nothing. A negative number throws `std::invalid_argument` (Python: `ValueError`).
- `request::connect_timeout(long seconds)`: the limit for resolving the host, connecting, the proxy handshake and the TLS handshake. `timeout()` keeps limiting each write and read. `0` (the default) means `timeout()` covers the connect steps too, as before. A negative number throws as above. In Python it takes seconds like `timeout()` and rounds up to at least 1.
- The same two names are bound in Python (`Request.max_redirects`, `Request.connect_timeout`) and accepted by `httpp.request(...)` and so by `get`, `post`, and the other helpers.
- No `user_agent()` or `referer()` method: `header()` is the one way. `httpp curl` gets `-A/--user-agent` and `-e/--referer` as sugar over `header()` (an explicit `-H` of the same name wins), plus `--max-redirs` (a whole number, 0 or more, exit code 47 when exceeded) and `--connect-timeout` (seconds, at least 1).
- `client` and `async_client` do not get the two settings yet. The fields live in the shared `session_settings`, so adding `set_max_redirects` and `set_connect_timeout` later is a few lines; it waits for someone to need it.

#### Consequences

- A connect that hangs can now be cut short without also cutting a slow answer; the redirect limit is the caller's.
- `-m/--max-time` in `httpp curl` is, and always was, a limit per step, not for the whole transfer as in curl: a server that keeps sending a byte now and then is never cut off. Its help text now says so. A deadline for the whole request is a different feature (the engine has no such timer) and is not part of this ADR.
- Not added and still missing from `httpp curl`: `-s`, `-S`, `-w`, `-O`, `-T`, `-D`, `-d @file`, `--compressed`, `--retry`, several URLs.

#### Related

ADR-0051 and 0077

### `request::max_redirects(n)` is the one redirect setting (0 = none); `auto_compression` on `httpp::request`

- **Status:** accepted
- **Decided by:** the project owner ("two concepts, `follow_redirects` and `max_redirects`, is not wanted"; "for zero it means do not follow"; "an `auto_compression(true)` is easy to add")
- **Source:** `include/httpp/client.hpp`, `src/core/client.cpp`, `src/core/http_exchange.cpp`, `src/core/internal/http_exchange.hpp`, `src/core/internal/async_http.hpp`, `src/core/async_http.cpp`, `src/core/url.cpp`, `src/bindings/python/cy/{declarations,transfer}.pxi`, `src/bindings/python/httpp/_http.py`, `src/cli/httpp_cli.cpp`
- **Follows:** ADR-0077 and the part "`httpp::request` gets `max_redirects` and `connect_timeout`"

#### Context

- `httpp::request` had `follow_redirects(bool)` and (the part "`httpp::request` gets `max_redirects` and `connect_timeout`") `max_redirects(n)`: two settings for one idea. A limit without `follow_redirects()` did nothing, and `follow_redirects().max_redirects(0)` meant "fail on the first redirect", which is a third behaviour nobody asked for.
- The client always sent `Accept-Encoding: gzip, deflate` and always decoded the body; there was no way to get the coded bytes.

#### Decision

- `request::max_redirects(int count)` is the only redirect setting of a request. `0`, the default, follows none: a 3xx is the answer. `n > 0` follows up to `n` hops; one more fails with `too_many_redirects`. `-1` follows without a limit and without an error, as in curl. Any other negative number throws `std::invalid_argument` (Python: `ValueError`).
- `request::follow_redirects` is removed (C++ and `Request` in Python). `client::set_follow_redirects`, `async_client::set_follow_redirects` and `download::follow_redirects` stay: they are session or transfer settings and are not part of this change.
- The Python helpers (`httpp.request`, `get`, `post`, ...) take `max_redirects=N`, and still accept `follow_redirects=True` as in `Client(...)`, which means `max_redirects=20`.
- `httpp curl -L` means `max_redirects(20)`, `-L --max-redirs N` means `max_redirects(N)` (and `-1` means no limit). `-L --max-redirs 0` keeps curl's behaviour (exit code 47 on the first redirect): the command checks the first head itself.
- `request::auto_compression(bool enable = true)` (Python: `Request.auto_compression`, `httpp.request(..., auto_compression=False)`). On, the default, is the old behaviour. Off: no `Accept-Encoding` is sent unless `header()` sets one, and a gzip / deflate body comes back exactly as the server sent it.
- `client` and `async_client` do not get these settings yet; the fields live in the shared `session_settings`.

#### Consequences

- With `-1` a redirect loop never ends, because `timeout` limits each step and not the whole request. A deadline for the whole request would close this; it is not part of this ADR.
- `follow_redirects()` on `request` is a breaking removal; the version is still 0.x and it is listed under **Changed**.
- `httpp curl` keeps `--compressed` as an accepted no-op: it streams the answer, so the request already asks for `Accept-Encoding: identity`. Making it real needs decoding in the streaming path, which is separate work.

#### Related

ADR-0077 and the part "`httpp::request` gets `max_redirects` and `connect_timeout`"
