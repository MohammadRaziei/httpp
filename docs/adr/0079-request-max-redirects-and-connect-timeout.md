# ADR-0079: `httpp::request` gets `max_redirects` and `connect_timeout`

- **Status:** accepted
- **Decided by:** the project owner ("enrich `httpp::request`, not a second class", then "agreed" to the first candidates)
- **Source:** `include/httpp/client.hpp`, `src/core/client.cpp`, `src/core/http_exchange.cpp`, `src/core/internal/http_exchange.hpp`, `src/core/internal/async_http.hpp`, `src/core/async_http.cpp`, `src/bindings/python/cy/{declarations,transfer}.pxi`, `src/bindings/python/httpp/_http.py`, `src/cli/httpp_cli.cpp`
- **Follows:** ADR-0077, ADR-0078

## Context

ADR-0077 and ADR-0078 settled that curl-like needs are met by enriching `httpp::request`, not by a second API. Looking at what `httpp curl` still lacked against what the engine could do:

- The redirect limit was a constant (`request_spec::redirect_limit`, 20); no caller could change it.
- `timeout` is one number for every step the engine takes (resolve, connect, write, each read). There was no way to say "fail fast when the host is unreachable, but let a slow answer take its time".
- `user_agent` and `referer` were on the first candidate list, but both are plain headers: `request::header("User-Agent", ...)` already sets them, and the engine only adds its default `User-Agent: httpp` when none is given. A method of their own would be a second way to do the same thing in C++ and again in Python.

## Decision

- `request::max_redirects(int count)`: how many redirects `follow_redirects()` may follow before the request fails with `too_many_redirects`. Default 20. `0` follows none (the first redirect fails). Without `follow_redirects()` it changes nothing. A negative number throws `std::invalid_argument` (Python: `ValueError`).
- `request::connect_timeout(long seconds)`: the limit for resolving the host, connecting, the proxy handshake and the TLS handshake. `timeout()` keeps limiting each write and read. `0` (the default) means `timeout()` covers the connect steps too, as before. A negative number throws as above. In Python it takes seconds like `timeout()` and rounds up to at least 1.
- The same two names are bound in Python (`Request.max_redirects`, `Request.connect_timeout`) and accepted by `httpp.request(...)` and so by `get`, `post`, and the other helpers.
- No `user_agent()` or `referer()` method: `header()` is the one way. `httpp curl` gets `-A/--user-agent` and `-e/--referer` as sugar over `header()` (an explicit `-H` of the same name wins), plus `--max-redirs` (a whole number, 0 or more, exit code 47 when exceeded) and `--connect-timeout` (seconds, at least 1).
- `client` and `async_client` do not get the two settings yet. The fields live in the shared `session_settings`, so adding `set_max_redirects` and `set_connect_timeout` later is a few lines; it waits for someone to need it.

## Consequences

- A connect that hangs can now be cut short without also cutting a slow answer; the redirect limit is the caller's.
- `-m/--max-time` in `httpp curl` is, and always was, a limit per step, not for the whole transfer as in curl: a server that keeps sending a byte now and then is never cut off. Its help text now says so. A deadline for the whole request is a different feature (the engine has no such timer) and is not part of this ADR.
- Not added and still missing from `httpp curl`: `-s`, `-S`, `-w`, `-O`, `-T`, `-D`, `-d @file`, `--compressed`, `--retry`, several URLs.

## Related

ADR-0053, ADR-0077, ADR-0078
