# ADR-0082: `swagger` takes the OpenAPI URL in its constructor and the base in `mount`

- **Status:** accepted; supersedes the Swagger UI part of ADR-0066 (`swagger_ui`, `set_docs_path`, `set_openapi_url`)
- **Decided by:** the project owner ("only `mount(s, base)`", "the argument is required, so it should be a required argument")
- **Source:** `include/httpp/swagger_ui.hpp`, `src/core/swagger_ui.cpp`, `src/bindings/python/cy/swagger_ui.pxi`, `tests/cpp/test_swagger_ui_route.cpp`, `tests/python/test_builders.py`

## Context

`directory` and `file_browser` take the place they are mounted at as the argument of `mount(s, base)`. `swagger_ui` had its own `set_docs_path`, a second place to say where it lives, and `swagger_ui()` with no argument looked like an object with no identity. The page also wrote its file addresses from the path, so a base given to `mount` would have been two sources of truth.

## Decision

- The class is `httpp::swagger` (Python: `httpp.Swagger`). The header and the asset names (`swagger_ui_asset`, `find_swagger_ui_asset`) keep `swagger_ui`: they are about the Swagger UI files.
- `swagger(openapi_url)`: the address of the OpenAPI document is a **required** constructor argument and is used as it is. The library neither makes nor serves that document (a Python `Server` does both), so there is no honest default; a guess would give an empty page. An empty address throws `std::invalid_argument` (Python: `ValueError`).
- `set_title(title)` stays a setter: it is a setting of the page, not a place.
- `mount(server s, const std::string& base = "/docs")`: the page is at `base`, its files under it. `base` must start with `/` and name more than the root, or `mount` throws `std::invalid_argument` before it mounts anything. The same object can be mounted on several bases, and the page follows the base it was mounted at.
- `set_docs_path` and `set_openapi_url` are removed (C++ and Python, including `SwaggerUI(docs_path=...)`).

## Rejected alternatives

- **`set_openapi_url` inside `mount(s, base, url)`.** Two strings in a row that are easy to swap, and it suggests that `mount` registers that route.
- **A default `/openapi.json`.** Optional-looking, and a silent empty page when the document is somewhere else.
- **Prefixing the OpenAPI address with `base`.** The document is served by the user, and the library cannot know where.
- **Allowing `base = "/"`.** The page would share the root with the application's own routes.

## Consequences

- A breaking change in C++ and Python for anyone who called `swagger_ui`, `set_docs_path` or `SwaggerUI(...)`; the CHANGELOG lists it under **Changed**.
- Python's `Server(docs_url=..., openapi_url=...)` is unchanged.

## Related

ADR-0066, ADR-0081, ADR-0008 (Swagger UI compiled in).
