# ADR-0103: Python constructors take the settings as keywords with the names of the setters; no module-level `httpp.get(...)`

- **Status:** implemented; the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-20
- **Decided by:** the project owner: the keywords are the names of the setters; `follow_redirects` goes; the `(user, password)` tuple stays; the one-call helpers go ("it is not clear whether it is a server or a client"); breaking changes are accepted
- **Source:** `src/bindings/python/cy/{client,transfer}.pxi`, `src/bindings/python/httpp/{_http,__init__,testclient}.py`, `tests/python/test_cpp_python_parity.py`
- **Follows:** ADR-0092, ADR-0100, ADR-0101

## Context

Python had two ways to say the same thing, with names that did not agree:

- The setters (`Request.bearer(t)`, `Request.basic_auth(u, p)`) and the constructor keywords of `Client` and `AsyncClient` (`token=`, `auth=`) named the same setting differently. `follow_redirects=True` was a third spelling of `max_redirects(20)`, and `Request` and `Download` took no keywords at all.
- `httpp.get(url)`, `httpp.post(url)` and the other module-level helpers (and `httpp.request(method, url)`, which clashed with the name of the C++ class `httpp::request`) were one-call shortcuts. The name `httpp.get` does not say whether it sends a request or declares a server route (`app.get`).

## Decision

1. **A keyword is the name of a setter and does what the setter does.** `Request(url, method="PUT", timeout=10, bearer=token)` is `Request(url).method("PUT").timeout(10).bearer(token)`. This holds for `Request`, `Download`, `Client` and `AsyncClient`. The constructor calls the public setters, so the rules (a negative `max_time` is a `ValueError`, a `proxy` that is not a URL string is an error) exist once.
2. **Only three differences from the setter names:**
   - the setters called once per item (`header`, `param`, `cookie`) take a dict under the plural name: `headers`, `params`, `cookies`;
   - `basic_auth` takes `(user, password)` as one tuple;
   - `json`, `form` and `data` set the body and exclude each other (`ValueError` if two are given).
3. **Renamed (no alias; breaking):** `Client(..., token=)` and `AsyncClient(..., token=)` are `bearer=`; `auth=` is `basic_auth=`.
4. **Removed:** `follow_redirects` on `Client` and `AsyncClient` and on the helpers. `max_redirects=N` follows up to N redirects (0 none, -1 no limit); the old `follow_redirects=True` is `max_redirects=20`.
5. **Removed:** `httpp.request`, `httpp.get`, `httpp.head`, `httpp.options`, `httpp.post`, `httpp.put`, `httpp.patch`, `httpp.delete`. The same call is `httpp.Request(url, method="GET", ...).run()`, one line too.
6. **`Request` and `Download` have the keywords too** (`Download(url, path, resume=True, timeout=30, bearer=token)`), which are the same names as the mixin of C++ (ADR-0094, ADR-0101).
7. **The parity test checks the rule:** the keywords of `Client` and `AsyncClient` equal the setter names (plural for the three), and `Request` and `Download` accept every keyword in `KWARGS`, each of them the name of a method.

## Rejected alternatives

- **Keep the helpers and rename `request` to `send`:** still a module-level `get` that could be a route. With keywords on `Request`, the shortcut saves nothing.
- **Keep `token` and `auth` as the familiar `requests` spelling:** one setting with two names, which is what this series of ADRs removes.
- **Keywords on the C++ constructors too:** a C++ constructor takes the arguments the object cannot do without (the URL); settings are the setters. Named arguments are the Python idiom for the same list of settings.

## Taken by default while implementing (owner to confirm)

- **`TestClient` is unchanged.** Its `get`, `post`, `request`, `auth=`, `follow_redirects=` follow Starlette's `TestClient` on purpose (a test written for FastAPI runs here), and it is a Python-only test helper. Inside it, the request is now built with `Request(...)`, not with the removed helper.
- **`httpp.download(url, path)` stays:** it is the one-call form of `Download`, and the name does not collide with a server route.
- **`follow_redirects=True` became `max_redirects=20` everywhere in the tests and examples** (the value it had).
- **`Client` and `AsyncClient` still have no setters in Python** (only the keywords, and `Client.header`). Adding them would be a separate step.

## Consequences

- Python code that used `httpp.get(...)` and the other helpers, `token=`, `auth=` or `follow_redirects=` must change (a `TypeError` or `AttributeError` says where).
- One list of names to remember: the setter and the keyword are the same word.
- `_http.py` holds only `AsyncClient`.

## Verified

Built on Linux and run: the whole Python suite against a wheel built from the same tree (the parity test with the new checks; all the tests that used the helpers, now `Request(...).run()`; the client, async client, download and `TestClient` tests).

## Not verified

Windows and macOS. Code outside this repository that calls the removed names. The docs site and README were searched for the old names and the redirect sentence in the README was rewritten; pages that show only server code were not read.

## Related

ADR-0092, ADR-0094, ADR-0100, ADR-0101
