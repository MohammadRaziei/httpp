# ADR-0100: the same names in C++ and Python, except where the language forces a difference

- **Status:** implemented; the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-19
- **Decided by:** the project owner, who asked for one name where there is no reason for two ("why should it differ?") and approved the list below
- **Source:** `ALIASES` in `tests/python/test_cpp_python_parity.py`, `include/httpp/{server,router,progress,client}.hpp`, `src/bindings/python/httpp/_http.py`, `src/bindings/python/cy/client.pxi`
- **Follows:** ADR-0016, ADR-0017, ADR-0092, ADR-0094

## Context

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

## Decision

1. **The first four rows get one name each, the C++ name changing where it is the odd one:**
   - `bar::finished()` (C++), as the getter rule of ADR-0092 says (no `is_` or `get_`); it stays a property in Python, which is only the way each language writes a getter.
   - `server::before_request(hook)` and `server::after_request(hook)`, as in Python. (The name is Flask's; FastAPI has no such pair.)
   - `router::websocket_routes()`, as in Python.
   - `send` in Python: `Client.send(method, path, ...)` and `AsyncClient.send(...)`, as in C++. `Client.request` and `AsyncClient.request` are gone, with no alias: the project has no deprecation (the owner's rule), so the two languages have one name, not two.
2. **The remaining differences stay, and each now has its reason here and in `ALIASES`:**
   - `del` and `delete`: `delete` is a C++ keyword and `del` a Python one, so no single name that is the HTTP verb exists in both. A third word (`remove`, `erase`) would work in both but hides that the call is DELETE, so it was not taken.
   - `*_async` versus one name: Python tells an `async def` handler from the function itself; C++ cannot overload on a `std::function` that differs only by what it promises, so it needs the suffix.
   - `use` and `add_middleware`: different meaning (ADR-0037).
3. **Not changed: the module-level `httpp.request(method, url, ...)` and `httpp.get(...)`.** They are the shorthand of the `requests` library and have no C++ twin by name (the C++ one-shot is `client::fetch` and the builder `httpp::request(url)`). If the aim is one vocabulary for them too, that is a separate decision.
   `TestClient.request` (a Python-only test helper) is not part of this either.

## Taken by default while implementing (owner to confirm)

- **`del` and `delete` stay** (the owner floated `remove` or `clear`, not decided).
- **No alias on either side,** as ADR-0062 did ("breaking for C++ code"). Both are renamed outright: the owner said there is no such thing as deprecated here, and the names are the same in both languages or they are not.
- **Which side changes:** the C++ names move to Python's for `finished`, `before_request`, `after_request` and `websocket_routes`; Python moves to C++'s for `send`. The reason for the split is that Python's names were the clearer ones in the first group, and `send` the clearer one (it is a verb, and `request` is also the name of the builder class) in the second.

## Consequences

- C++ callers of `bar::is_finished`, `server::before`, `server::after` and `router::ws_routes` must rename them (a compile error says where).
- Python callers of `Client.request` and `AsyncClient.request` must rename to `send` (an `AttributeError` says where).
- The `ALIASES` of the parity test shrinks to the rows with a reason: `del`, `use`, and the `*_async` family.
- `make_request` (ADR-0094) and `clone` (ADR-0093) are the same in both languages.

## Verified

Built on Linux and run: the whole C++ suite, and the Python suite against a wheel built from the same tree, including `test_send_is_the_name_of_the_send_call_in_python_and_in_cpp` and the parity test (names found on the Python side for the renamed C++ names).

## Not verified

Windows and macOS. Code outside this repository that calls the old C++ names. The docs site was searched for the old names and the table in `docs/site/docs/server/hooks.mdx` was updated; pages that show only Python were not read.

## Related

ADR-0016, ADR-0017, ADR-0037, ADR-0062, ADR-0092, ADR-0093, ADR-0094
