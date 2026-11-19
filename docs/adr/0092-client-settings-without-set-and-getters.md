# ADR-0092: client-side settings have no `set_` prefix; the same name with no argument reads the value

- **Status:** accepted
- **Decided by:** the project owner ("make the API uniform: `set_` in one place and not in another; pick one"; "a getter has the same name but takes no argument"; "drop `follow_redirects`: `max_redirects` for them too")
- **Source:** `include/httpp/client.hpp`, `include/httpp/download.hpp`, `src/core/client.cpp`, `src/core/download.cpp`, `src/core/internal/http_exchange.hpp`, `src/bindings/python/cy/{declarations,client,transfer}.pxi`, `src/bindings/python/httpp/_http.py`, `tests/python/test_cpp_python_parity.py`
- **Follows:** ADR-0079, ADR-0084, ADR-0089

## Context

`request` (the builder) already used bare names (`timeout(30)`, `verify(false)`, `proxy(url)`), while `client`, `async_client` and `download` spelled the same settings `set_timeout`, `set_verify`, `set_proxy`, and redirects were `set_follow_redirects(bool)` on the sessions and `follow_redirects(bool)` on `download` next to `request::max_redirects(n)` (ADR-0089). Someone moving from a request to a session had to learn a second spelling for every setting, and a setting had no getter.

## Decision

- In the client family (`request`, `client`, `async_client`, `download`) a setting is **named by what it is**, without `set_`. Called **with an argument** it sets and returns `*this`; the **same name with no argument** reads the value (`const`): `c.timeout(30)` sets, `c.timeout()` returns 30.
- `client` and `async_client` now carry the settings of `request` under the same names: `header`, `timeout`, `connect_timeout`, `max_time`, `max_redirects`, `auto_compression`, `basic_auth`, `bearer` (was `set_bearer_token`; `request` already said `bearer`), `proxy`, `verify`, `ca_file`, and `keep_alive` (`client` only, as before). `connect_timeout`, `max_time` and `auto_compression` are new on the sessions; the fields were already in the shared `session_settings`.
- Redirects: `follow_redirects` is gone from sessions and `download`; they take `max_redirects(int)` with the rules of ADR-0089 (0 follows none, -1 has no limit, any other negative number throws `std::invalid_argument`). A session follows none by default; `download` keeps its default of following (20).
- Getters exist for the scalar settings (`timeout`, `connect_timeout`, `max_time`, `max_redirects`, `auto_compression`, `verify`, `ca_file`, `keep_alive`; on `download`: `max_redirects`, `timeout`, `max_wait`). There are none for `header` (several values, and a default may replace one), `basic_auth` and `bearer` (secrets), and `proxy` (the parsed form is not the URL that was given). `request` is a builder and has no getters.
- Because the no-argument form is a getter, `auto_compression(bool)` has no default argument on `request` either: `auto_compression()` on a session must not mean "turn it on".
- `timeout(n)` with `n <= 0` now means "the default" on both sessions (the blocking client used to ignore it, the async one stored it). `connect_timeout` and `max_time` throw `std::invalid_argument` for a negative number, as on `request`.
- Python: `Client`, `AsyncClient` and `Download` follow the same names. `Client.set_header` is `Client.header`; `Download.set_timeout`, `set_max_wait` and `follow_redirects` are `timeout`, `max_wait` and `max_redirects`, and with no argument they return the value (`max_wait` in seconds). `Client(...)` and `AsyncClient(...)` take `max_redirects`, `connect_timeout`, `max_time` and `auto_compression` as keywords, and keep `follow_redirects=True` as the shorthand for `max_redirects=20`.

## Not part of this ADR

The server side (`server`, `directory`, `file_browser`, `swagger`, `cors` and the other middleware, `cookie`) keeps `set_...` for its settings. They are configured once at start-up, not chained like a request, and some of the bare names would collide inside their classes with types (`path`, `view`, `sort`). Whether they should follow is a separate decision, and a rename that wide needs a build to catch name clashes. (Done in ADR-0096.)

## Consequences

- Breaking for C++ code that calls `client::set_*`, `async_client::set_*` or `download::set_timeout`, `set_max_wait`, `follow_redirects`, and for Python code that calls `Client.set_header`, `Download.set_timeout`, `Download.set_max_wait` or `Download.follow_redirects`. The version is still 0.x. The old ADRs keep the names they used then.
- `httpp curl` does not use these classes and is unchanged.

## Related

ADR-0079, ADR-0084, ADR-0089
