# ADR-0092: Setting names: no `set_` prefix, the same names in C++ and Python, keywords as setter names

- **Status:** accepted / implemented (merged ADR; each part keeps its own status line)
- **Merged on:** 2026-11-21, at the request of the project owner: the decisions of one subject were spread over 4 ADRs. The text of every part is kept, as a section below, in the order it was decided.

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history. Where a part and this summary disagree, this summary wins.

- **No configuration setter has a `set_` prefix.** `timeout(30)` sets, `timeout()` reads (client family); the server side (`server`, `directory`, `file_browser`, `swagger`, the middleware, `cookie`, `websocket`) dropped `set_` too, and `set_view`, `set_sort`, `set_same_site` became `layout`, `sort_by`, `same_site_policy`.
- **Kept on purpose:** `set_cookie` and `set_header` on a response (they are actions on one response), the logging functions, `bar::set_progress` and the Python-only `Server.set_*_hook`.
- **Redirects are `max_redirects(int)` everywhere** (see ADR-0079).
- **One name in C++ and Python** wherever the language does not force two: `finished`, `before_request`, `after_request`, `websocket_routes`, `send`. The remaining differences are listed in the parts with their reasons. No aliases, no deprecation period: both sides were renamed outright.
- **Python constructors** (`Request`, `Download`, `Client`, `AsyncClient`) take the settings as keywords with the names of the setters; `token` and `auth` became `bearer` and `basic_auth`; `follow_redirects` is gone; the module-level `httpp.get(...)` helpers are removed.
- How the client classes hold these settings is ADR-0093.

## Parts
### client-side settings have no `set_` prefix; the same name with no argument reads the value

- **Status:** accepted
- **Decided by:** the project owner ("make the API uniform: `set_` in one place and not in another; pick one"; "a getter has the same name but takes no argument"; "drop `follow_redirects`: `max_redirects` for them too")
- **Source:** `include/httpp/client.hpp`, `include/httpp/download.hpp`, `src/core/client.cpp`, `src/core/download.cpp`, `src/core/internal/http_exchange.hpp`, `src/bindings/python/cy/{declarations,client,transfer}.pxi`, `src/bindings/python/httpp/_http.py`, `tests/python/test_cpp_python_parity.py`
- **Follows:** ADR-0079 and 0077

#### Context

`request` (the builder) already used bare names (`timeout(30)`, `verify(false)`, `proxy(url)`), while `client`, `async_client` and `download` spelled the same settings `set_timeout`, `set_verify`, `set_proxy`, and redirects were `set_follow_redirects(bool)` on the sessions and `follow_redirects(bool)` on `download` next to `request::max_redirects(n)` (ADR-0079). Someone moving from a request to a session had to learn a second spelling for every setting, and a setting had no getter.

#### Decision

- In the client family (`request`, `client`, `async_client`, `download`) a setting is **named by what it is**, without `set_`. Called **with an argument** it sets and returns `*this`; the **same name with no argument** reads the value (`const`): `c.timeout(30)` sets, `c.timeout()` returns 30.
- `client` and `async_client` now carry the settings of `request` under the same names: `header`, `timeout`, `connect_timeout`, `max_time`, `max_redirects`, `auto_compression`, `basic_auth`, `bearer` (was `set_bearer_token`; `request` already said `bearer`), `proxy`, `verify`, `ca_file`, and `keep_alive` (`client` only, as before). `connect_timeout`, `max_time` and `auto_compression` are new on the sessions; the fields were already in the shared `session_settings`.
- Redirects: `follow_redirects` is gone from sessions and `download`; they take `max_redirects(int)` with the rules of ADR-0079 (0 follows none, -1 has no limit, any other negative number throws `std::invalid_argument`). A session follows none by default; `download` keeps its default of following (20).
- Getters exist for the scalar settings (`timeout`, `connect_timeout`, `max_time`, `max_redirects`, `auto_compression`, `verify`, `ca_file`, `keep_alive`; on `download`: `max_redirects`, `timeout`, `max_wait`). There are none for `header` (several values, and a default may replace one), `basic_auth` and `bearer` (secrets), and `proxy` (the parsed form is not the URL that was given). `request` is a builder and has no getters.
- Because the no-argument form is a getter, `auto_compression(bool)` has no default argument on `request` either: `auto_compression()` on a session must not mean "turn it on".
- `timeout(n)` with `n <= 0` now means "the default" on both sessions (the blocking client used to ignore it, the async one stored it). `connect_timeout` and `max_time` throw `std::invalid_argument` for a negative number, as on `request`.
- Python: `Client`, `AsyncClient` and `Download` follow the same names. `Client.set_header` is `Client.header`; `Download.set_timeout`, `set_max_wait` and `follow_redirects` are `timeout`, `max_wait` and `max_redirects`, and with no argument they return the value (`max_wait` in seconds). `Client(...)` and `AsyncClient(...)` take `max_redirects`, `connect_timeout`, `max_time` and `auto_compression` as keywords, and keep `follow_redirects=True` as the shorthand for `max_redirects=20`.

#### Not part of this ADR

The server side (`server`, `directory`, `file_browser`, `swagger`, `cors` and the other middleware, `cookie`) keeps `set_...` for its settings. They are configured once at start-up, not chained like a request, and some of the bare names would collide inside their classes with types (`path`, `view`, `sort`). Whether they should follow is a separate decision, and a rename that wide needs a build to catch name clashes. (Done in the part "server-side settings have no `set_` prefix either".)

#### Consequences

- Breaking for C++ code that calls `client::set_*`, `async_client::set_*` or `download::set_timeout`, `set_max_wait`, `follow_redirects`, and for Python code that calls `Client.set_header`, `Download.set_timeout`, `Download.set_max_wait` or `Download.follow_redirects`. The version is still 0.x. The old ADRs keep the names they used then.
- `httpp curl` does not use these classes and is unchanged.

#### Related

ADR-0079 and 0077

### server-side settings have no `set_` prefix either

- **Status:** accepted
- **Decided by:** the project owner ("maybe the name can be changed altogether, `output_path` or `sorting` or `enable_sort` or whatever"), after the part "client-side settings have no `set_` prefix; the same name with no argument reads the value" left the server side behind
- **Source:** `include/httpp/{server,file_browser,swagger,cookie,middleware,websocket}.hpp` and their sources under `src/core`, the Cython bindings (`src/bindings/python/cy/*.pxi`) and the Python layer (`server.py`, `_run.py`, `middleware.py`), `tests/`, `docs/site`
- **Follows:** ADR-0093 and the part "client-side settings have no `set_` prefix; the same name with no argument reads the value"

#### Context

the part "client-side settings have no `set_` prefix; the same name with no argument reads the value" removed `set_` from the client family and left `server`, `directory`, `file_browser`, `swagger`, the middleware classes and `cookie` on `set_...`, because some bare names might clash with types inside their classes. That left the library with two spellings for one thing, depending on which side of the wire you are on.

#### Decision

- Every configuration setter of those classes loses its `set_` prefix: `server.read_timeout(30)`, `file_browser.title("Files")`, `cors.allow_origins({...})`. It returns `*this` as before. The same name with no argument is reserved for a getter (the part "client-side settings have no `set_` prefix; the same name with no argument reads the value"); none of these classes has one yet.
- Three names change beyond dropping `set_`, because the bare name is taken by a type or says too little:

| before | after | why |
|---|---|---|
| `file_browser::set_view(view_mode)` | `layout(view_mode)` | `view_mode` is the type; `view` alone is vague |
| `file_browser::set_sort(sort_key)` | `sort_by(sort_key)` | it names the action, `sort_key` is the type |
| `cookie::set_same_site(same_site)` | `same_site_policy(same_site)` | `same_site` is the enum |

- Everything else is the old name without `set_`: `server` (`error_handler`, `exception_handler`, `tls`, `thread_pool`, `read_timeout`, `write_timeout`, `keep_alive_timeout`, `keep_alive_max`, `max_body`, `header_timeout`, `body_timeout`, `max_connections`, `async_timeout`, `compression`, `reuse_port`, `io_threads`, `request_rewrite`), `directory` (`index`, `show_hidden`, `show_dates`, `download_active`, `inline_active`), `file_browser` (`title`, `theme`, `accent`, `search`, `show_hidden`, `show_dates`, `max_entries`, `safe_mode`, `zip`, `powered_by`, `max_zip_entries`), `swagger` (`title`), `cookie` (`path`, `domain`, `max_age`, `secure`, `http_only`), the middleware (`cors`: `allow_origins`, `allow_origin_regex`, `allow_methods`, `allow_headers`, `allow_credentials`, `expose_headers`, `max_age`; `trusted_host`: `allowed_hosts`, `www_redirect`; `https_redirect`: `trust_forwarded_proto`; `forwarded_headers`: `trusted_proxies`, `trust_forwarded_host`; `basic_auth`: `realm`, `users`, `path_prefix`, `verifier`; `rate_limit`: `rate`, `burst`, `max_clients`) and `websocket` (`async_timeout`).
- Python follows the C++ names (`Server.read_timeout(30)`, `Server.tls(cert, key)`, `Directory.index(...)`), with the existing aliases for the names that already differed.
- Constructors whose parameter has the name of a setter call it as `this->name(...)` (`trusted_host`, `basic_auth`).

#### Not renamed

- `set_cookie` and `set_header` on a response: they are actions on one response (`set_header` replaces, `add_header` appends), not configuration.
- The logging functions (`set_level`, `set_log_level`, `set_log_file`, `set_log_system`, `set_log_callback`, `set_formatter`, `set_propagate`, `set_serialized`) imitate Python's `logging` (ADR-0011), and `bar::set_progress` / `bar::set_total` is a position, not a setting.
- Python-only internals `Server.set_before_hook`, `set_after_hook`, `set_error_hook`, which have no C++ counterpart.

#### Consequences

- Breaking for every user of the names above, in C++ and in Python. The version is still 0.x. Old ADRs keep the names they used then.
- Getters for these classes are still to come, one by one where they are useful.

#### Related

ADR-0093 and the part "client-side settings have no `set_` prefix; the same name with no argument reads the value"

### the same names in C++ and Python, except where the language forces a difference

- **Status:** implemented; the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-19
- **Decided by:** the project owner, who asked for one name where there is no reason for two ("why should it differ?") and approved the list below
- **Source:** `ALIASES` in `tests/python/test_cpp_python_parity.py`, `include/httpp/{server,router,progress,client}.hpp`, `src/bindings/python/httpp/_http.py`, `src/bindings/python/cy/client.pxi`
- **Follows:** ADR-0016 and 0093 and the part "client-side settings have no `set_` prefix; the same name with no argument reads the value"

#### Context

ADR-0016 says a capability should be reachable from C++ and Python under the same name, and keeps an allow-list of intentional differences (`ALIASES`). Reading that list for this ADR, most entries had **no recorded reason**:

| C++ | Python | Reason found |
|---|---|---|
| `client::send` | `Client.request` | none; the name follows the `requests` library |
| `server::before`, `server::after` | `before_request`, `after_request` | none |
| `bar::is_finished()` | property `finished` | none (the note says "like `total` and `current`") |
| `router::ws_routes()` | `websocket_routes()` | none |
| `del` | `delete` | a language rule: `del` is a keyword in Python, `delete` one in C++ |
| `get_async`, `route_async`, ... | the ordinary `get`, `route` with an `async def` | a language rule (see below) |
| `use` | `add_middleware` | on purpose (ADR-0037): `use` takes raw C++ hooks, `add_middleware` installs a ready-made Python class |

A difference with no reason is a cost for whoever moves between the two languages, and the parity test could not tell a good one from an accident.

#### Decision

1. **The first four rows get one name each, the C++ name changing where it is the odd one:**
   - `bar::finished()` (C++), as the getter rule of the part "client-side settings have no `set_` prefix; the same name with no argument reads the value" says (no `is_` or `get_`); it stays a property in Python, which is only the way each language writes a getter.
   - `server::before_request(hook)` and `server::after_request(hook)`, as in Python. (The name is Flask's; FastAPI has no such pair.)
   - `router::websocket_routes()`, as in Python.
   - `send` in Python: `Client.send(method, path, ...)` and `AsyncClient.send(...)`, as in C++. `Client.request` and `AsyncClient.request` are gone, with no alias: the project has no deprecation (the owner's rule), so the two languages have one name, not two.
2. **The remaining differences stay, and each now has its reason here and in `ALIASES`:**
   - `del` and `delete`: `delete` is a C++ keyword and `del` a Python one, so no single name that is the HTTP verb exists in both. A third word (`remove`, `erase`) would work in both but hides that the call is DELETE, so it was not taken.
   - `*_async` versus one name: Python tells an `async def` handler from the function itself; C++ cannot overload on a `std::function` that differs only by what it promises, so it needs the suffix.
   - `use` and `add_middleware`: different meaning (ADR-0037).
3. **Not changed: the module-level `httpp.request(method, url, ...)` and `httpp.get(...)`.** They are the shorthand of the `requests` library and have no C++ twin by name (the C++ one-shot is `client::fetch` and the builder `httpp::request(url)`). If the aim is one vocabulary for them too, that is a separate decision.
   `TestClient.request` (a Python-only test helper) is not part of this either.

#### Taken by default while implementing (owner to confirm)

- **`del` and `delete` stay** (the owner floated `remove` or `clear`, not decided).
- **No alias on either side,** as ADR-0062 did ("breaking for C++ code"). Both are renamed outright: the owner said there is no such thing as deprecated here, and the names are the same in both languages or they are not.
- **Which side changes:** the C++ names move to Python's for `finished`, `before_request`, `after_request` and `websocket_routes`; Python moves to C++'s for `send`. The reason for the split is that Python's names were the clearer ones in the first group, and `send` the clearer one (it is a verb, and `request` is also the name of the builder class) in the second.

#### Consequences

- C++ callers of `bar::is_finished`, `server::before`, `server::after` and `router::ws_routes` must rename them (a compile error says where).
- Python callers of `Client.request` and `AsyncClient.request` must rename to `send` (an `AttributeError` says where).
- The `ALIASES` of the parity test shrinks to the rows with a reason: `del`, `use`, and the `*_async` family.
- `make_request` (ADR-0093) and `clone` (ADR-0093) are the same in both languages.

#### Verified

Built on Linux and run: the whole C++ suite, and the Python suite against a wheel built from the same tree, including `test_send_is_the_name_of_the_send_call_in_python_and_in_cpp` and the parity test (names found on the Python side for the renamed C++ names).

#### Not verified

Windows and macOS. Code outside this repository that calls the old C++ names. The docs site was searched for the old names and the table in `docs/site/docs/server/hooks.mdx` was updated; pages that show only Python were not read.

#### Related

ADR-0016, 0037, 0062 and 0093 and the part "client-side settings have no `set_` prefix; the same name with no argument reads the value"

### Python constructors take the settings as keywords with the names of the setters; no module-level `httpp.get(...)`

- **Status:** implemented; the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-20
- **Decided by:** the project owner: the keywords are the names of the setters; `follow_redirects` goes; the `(user, password)` tuple stays; the one-call helpers go ("it is not clear whether it is a server or a client"); breaking changes are accepted
- **Source:** `src/bindings/python/cy/{client,transfer}.pxi`, `src/bindings/python/httpp/{_http,__init__,testclient}.py`, `tests/python/test_cpp_python_parity.py`
- **Follows:** ADR-0093, the part "client-side settings have no `set_` prefix; the same name with no argument reads the value" and the part "the same names in C++ and Python, except where the language forces a difference"

#### Context

Python had two ways to say the same thing, with names that did not agree:

- The setters (`Request.bearer(t)`, `Request.basic_auth(u, p)`) and the constructor keywords of `Client` and `AsyncClient` (`token=`, `auth=`) named the same setting differently. `follow_redirects=True` was a third spelling of `max_redirects(20)`, and `Request` and `Download` took no keywords at all.
- `httpp.get(url)`, `httpp.post(url)` and the other module-level helpers (and `httpp.request(method, url)`, which clashed with the name of the C++ class `httpp::request`) were one-call shortcuts. The name `httpp.get` does not say whether it sends a request or declares a server route (`app.get`).

#### Decision

1. **A keyword is the name of a setter and does what the setter does.** `Request(url, method="PUT", timeout=10, bearer=token)` is `Request(url).method("PUT").timeout(10).bearer(token)`. This holds for `Request`, `Download`, `Client` and `AsyncClient`. The constructor calls the public setters, so the rules (a negative `max_time` is a `ValueError`, a `proxy` that is not a URL string is an error) exist once.
2. **Only three differences from the setter names:**
   - the setters called once per item (`header`, `param`, `cookie`) take a dict under the plural name: `headers`, `params`, `cookies`;
   - `basic_auth` takes `(user, password)` as one tuple;
   - `json`, `form` and `data` set the body and exclude each other (`ValueError` if two are given).
3. **Renamed (no alias; breaking):** `Client(..., token=)` and `AsyncClient(..., token=)` are `bearer=`; `auth=` is `basic_auth=`.
4. **Removed:** `follow_redirects` on `Client` and `AsyncClient` and on the helpers. `max_redirects=N` follows up to N redirects (0 none, -1 no limit); the old `follow_redirects=True` is `max_redirects=20`.
5. **Removed:** `httpp.request`, `httpp.get`, `httpp.head`, `httpp.options`, `httpp.post`, `httpp.put`, `httpp.patch`, `httpp.delete`. The same call is `httpp.Request(url, method="GET", ...).run()`, one line too.
6. **`Request` and `Download` have the keywords too** (`Download(url, path, resume=True, timeout=30, bearer=token)`), which are the same names as the mixin of C++ (ADR-0093).
7. **The parity test checks the rule:** the keywords of `Client` and `AsyncClient` equal the setter names (plural for the three), and `Request` and `Download` accept every keyword in `KWARGS`, each of them the name of a method.

#### Rejected alternatives

- **Keep the helpers and rename `request` to `send`:** still a module-level `get` that could be a route. With keywords on `Request`, the shortcut saves nothing.
- **Keep `token` and `auth` as the familiar `requests` spelling:** one setting with two names, which is what this series of ADRs removes.
- **Keywords on the C++ constructors too:** a C++ constructor takes the arguments the object cannot do without (the URL); settings are the setters. Named arguments are the Python idiom for the same list of settings.

#### Taken by default while implementing (owner to confirm)

- **`TestClient` is unchanged.** Its `get`, `post`, `request`, `auth=`, `follow_redirects=` follow Starlette's `TestClient` on purpose (a test written for FastAPI runs here), and it is a Python-only test helper. Inside it, the request is now built with `Request(...)`, not with the removed helper.
- **`httpp.download(url, path)` stays:** it is the one-call form of `Download`, and the name does not collide with a server route.
- **`follow_redirects=True` became `max_redirects=20` everywhere in the tests and examples** (the value it had).
- **`Client` and `AsyncClient` still have no setters in Python** (only the keywords, and `Client.header`). Adding them would be a separate step.

#### Consequences

- Python code that used `httpp.get(...)` and the other helpers, `token=`, `auth=` or `follow_redirects=` must change (a `TypeError` or `AttributeError` says where).
- One list of names to remember: the setter and the keyword are the same word.
- `_http.py` holds only `AsyncClient`.

#### Verified

Built on Linux and run: the whole Python suite against a wheel built from the same tree (the parity test with the new checks; all the tests that used the helpers, now `Request(...).run()`; the client, async client, download and `TestClient` tests).

#### Not verified

Windows and macOS. Code outside this repository that calls the removed names. The docs site and README were searched for the old names and the redirect sentence in the README was rewritten; pages that show only server code were not read.

#### Related

ADR-0093, the part "client-side settings have no `set_` prefix; the same name with no argument reads the value" and the part "the same names in C++ and Python, except where the language forces a difference"
