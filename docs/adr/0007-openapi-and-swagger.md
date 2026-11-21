# ADR-0007: OpenAPI and Swagger UI: one request plan, one C++ writer, Swagger UI compiled in, `swagger` as a mountable

- **Status:** accepted (merged ADR; each part keeps its own status line)
- **Merged on:** 2026-11-21, at the request of the project owner: the decisions of one subject were spread over 5 ADRs. The text of every part is kept, as a section below, in the order it was decided.

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history. Where a part and this summary disagree, this summary wins.

- **Python builds OpenAPI from the same request plan** that decides where each handler argument comes from (`_params.py`), so the documentation and the real behaviour cannot drift apart.
- **One OpenAPI document writer in C++:** `httpp::openapi::document` (`httpp/openapi.hpp`) is a writer: C++ calls describe the API and `to_json()` returns the text. It has no JSON parser and reads no routes. Python uses the same class (`httpp.openapi.Document`); parameters, bodies, responses and model schemas still come from type hints in Python.
- **Swagger UI is vendored and compiled into the library** (5.33.x, one generated header per file by `embed_files`): `/docs` works offline, with no CDN and no data files.
- **`httpp::swagger(openapi_url)`** (Python `httpp.Swagger`) takes the address of the OpenAPI document as a required constructor argument and the base in `mount(server, base)`; the header is `swagger.hpp`. `directory`, `file_browser` and `swagger` derive from `httpp::mountable` (`server mount(server s, const std::string& base) const`, no default for `base` in the base class).

## Parts
### OpenAPI is built from the same request plan

- **Status:** accepted
- **Source:** `src/bindings/python/httpp/_params.py`, `_schema.py`, `handoff.md`

#### Context

If documentation and real behavior come from two sources, they drift apart sooner or later.

#### Decision

When a route is registered, `_params.py` decides where each handler argument comes from, using FastAPI's rules: a name inside the path is a
path parameter (`{x}`, `<x>`, `:x`); a scalar or a list of scalars is a query parameter; anything else is the JSON body (one body).
`Query`, `Path`, `Header`, `Cookie`, `Body` and `Depends` enrich the same plan. **That same plan** builds both the request path (the 422 error as
`{"detail": [...]}`) and the `/openapi.json` document (OpenAPI 3.1). `_schema.py` runs once and in plain Python.

#### Consequences

- Documentation and behavior cannot drift apart.
- The 422 for `int`/`float` path parameters still has the old `{"error": ...}` body, because an existing test depends on it
  (suggestion: change the test and move everything to `{"detail": [...]}`).
- If a route's own parameters are invalid, its dependencies are not run (only those errors are reported).
- Cleanup of `yield` dependencies happens after the handler and **before** the response is written (FastAPI's order differs slightly).
- Not there: forms and files, `Security()` (the FastAPI dependency that also extracts credentials), `servers`, response `examples`. Security schemes are declared for documentation only: `httpp.security.basic()` / `bearer()` / `api_key()`, `Server(security_schemes=..., security=...)`, a route's `security=`; the check itself stays with `BasicAuthMiddleware` or the caller's own code.

#### Related

ADR-0006 and the part "Swagger UI is compiled into the library (no CDN, no data files)"

### Swagger UI is compiled into the library (no CDN, no data files)

- **Status:** accepted
- **Source:** `cmake/EmbedFile.cmake`, `src/core/swagger.cpp`, `src/third_party/swagger-ui/VENDORED.md`

#### Context

`/docs` must work offline (no CDN), and the installed package must not contain a folder of `.js/.css/.png` files.

#### Decision

- Swagger UI 5.33.x is **vendored** in `src/third_party/swagger-ui/` (Apache-2.0).
- `embed_files(...)` turns each file into **one** generated header at configure time: `inline constexpr unsigned char n[]`
  in `.rodata`, one literal per **32 bytes**, each byte written as `\xNN` (the JS bundle is one 1.5 MB line and the PNG contains NUL bytes).
  The header is rewritten only when the input changes (`configure_file`), and `CMAKE_CONFIGURE_DEPENDS` tracks source changes.
- C++: `httpp::find_swagger_ui_asset(name)` and `httpp::mount_swagger_ui(server&, docs_path, openapi_url, title)`.
  Python: `Server.mount_swagger_ui` (the page and the three files are served from C++ without the GIL).

#### Rejected alternatives

A CDN (not offline), a data folder next to the wheel, `inline const std::string` (every process copied 1.6 MB onto the heap).
ReDoc was not added (a second bundle of about 1 MB).

#### Consequences

- Each request copies the body (`ponytail:`; upgrade path: a static `string_view` body inside `response`).
- A `-Woverlength-strings` warning for the multi-megabyte literals (not an error; suggestion: make `HTTPP_EMBED_AS_ARRAY` the default).
- `/docs` has only been seen in Chromium.
- Evaluated and **not decided:** a lighter replacement. Bundle file size (raw / gzip -9): Swagger 1550 / 420 KB
  (+ CSS 181 KB); RapiDoc 524 / 135 KB; Stoplight Elements 2035 / 599 KB; Redoc 2236 / 634 KB; Scalar 4512 / 1303 KB.
  RapiDoc's compatibility with the current `openapi.json` and its look have not been tried.

#### Related

the part "OpenAPI is built from the same request plan"

### `swagger` takes the OpenAPI URL in its constructor and the base in `mount`

- **Status:** accepted; supersedes the Swagger UI part of ADR-0066 (`swagger_ui`, `set_docs_path`, `set_openapi_url`)
- **Decided by:** the project owner ("only `mount(s, base)`", "the argument is required, so it should be a required argument")
- **Source:** `include/httpp/swagger.hpp`, `src/core/swagger.cpp`, `src/bindings/python/cy/swagger.pxi`, `tests/cpp/test_swagger_ui_route.cpp`, `tests/python/test_builders.py`

#### Context

`directory` and `file_browser` take the place they are mounted at as the argument of `mount(s, base)`. `swagger_ui` had its own `set_docs_path`, a second place to say where it lives, and `swagger_ui()` with no argument looked like an object with no identity. The page also wrote its file addresses from the path, so a base given to `mount` would have been two sources of truth.

#### Decision

- The class is `httpp::swagger` (Python: `httpp.Swagger`). The header and the asset names (`swagger_ui_asset`, `find_swagger_ui_asset`) keep `swagger_ui`: they are about the Swagger UI files.
- `swagger(openapi_url)`: the address of the OpenAPI document is a **required** constructor argument and is used as it is. The library neither makes nor serves that document (a Python `Server` does both), so there is no honest default; a guess would give an empty page. An empty address throws `std::invalid_argument` (Python: `ValueError`).
- `set_title(title)` stays a setter: it is a setting of the page, not a place.
- `mount(server s, const std::string& base = "/docs")`: the page is at `base`, its files under it. `base` must start with `/` and name more than the root, or `mount` throws `std::invalid_argument` before it mounts anything. The same object can be mounted on several bases, and the page follows the base it was mounted at.
- `set_docs_path` and `set_openapi_url` are removed (C++ and Python, including `SwaggerUI(docs_path=...)`).

#### Rejected alternatives

- **`set_openapi_url` inside `mount(s, base, url)`.** Two strings in a row that are easy to swap, and it suggests that `mount` registers that route.
- **A default `/openapi.json`.** Optional-looking, and a silent empty page when the document is somewhere else.
- **Prefixing the OpenAPI address with `base`.** The document is served by the user, and the library cannot know where.
- **Allowing `base = "/"`.** The page would share the root with the application's own routes.

#### Consequences

- A breaking change in C++ and Python for anyone who called `swagger_ui`, `set_docs_path` or `SwaggerUI(...)`; the CHANGELOG lists it under **Changed**.
- Python's `Server(docs_url=..., openapi_url=...)` is unchanged.

#### Related

ADR-0066 and 0081 and the part "Swagger UI is compiled into the library (no CDN, no data files)" (Swagger UI compiled in).

### one OpenAPI document writer in C++, used by C++ and by Python

- **Status:** accepted
- **Decided by:** the project owner ("implement what needs no JSON parser and no hack, in C++ and in Python")
- **Source:** `include/httpp/openapi.hpp`, `src/core/openapi.cpp`, `src/bindings/python/cy/openapi.pxi`, `src/bindings/python/httpp/openapi.py`, `_schema.py`, `tests/cpp/test_openapi.cpp`, `tests/python/test_openapi_document.py`
- **Related:** the part "OpenAPI is built from the same request plan" (the Python document is built from the request plan; unchanged), ADR-0016 (C++ was not required to generate OpenAPI), ADR-0016 (what is bound), the part "`swagger` takes the OpenAPI URL in its constructor and the base in `mount`" (`swagger` serves the page, not the document)

#### Context

ADR-0016 says C++ needs no OpenAPI generator, and that stays true for the part that needs reflection: reading the arguments of a handler. But a C++ user who wants the Swagger page still had to write the document as a raw string, with their own escaping, and the rules (which security names exist, how paths and methods group, what `security: []` means) lived only in Python's `_schema.py`.

#### Decision

- `httpp::openapi::document` (header `httpp/openapi.hpp`, not part of `httpp.h`) is a **writer**: C++ calls describe the API and `to_json()` returns compact JSON text. It checks what it can check without parsing: security names against the declared schemes, duplicate operations and operation ids, parameter places, a required path parameter, reserved keys given to `raw()`, valid UTF-8.
- It has **no JSON parser**. Anything bigger than a few fields (a schema, a parameter object, a request body, a response `content`) is JSON text that the caller supplies and the library puts in as it is. The library refuses only text that is blank or not UTF-8. That the text is valid JSON is the caller's promise, written in the header and in the docs.
- It reads **no routes**: the document is described apart from the handlers. The docs say plainly that it can drift. `server` is not changed.
- `httpp::openapi::{basic, bearer, api_key, raw_scheme}` build scheme objects; `any_of({...})` and `operation::no_security()` cover the common requirements.
- Python uses the same class: `httpp.openapi.Document`. `_schema.build_openapi_json` builds the operations as before (parameters, bodies, responses and model schemas still come from type hints, in Python: ADR-0016 rule 4) and hands them to `Document`, which assembles the paths, validates the security names and writes the text. `Server.openapi()` returns `json.loads` of that text and `/openapi.json` serves the text itself, written once and kept.
- `httpp.security.basic/bearer/api_key` keep returning dicts (their shape is tested and public); the C++ factories of the same names are the C++ face of them. `raw_scheme` is `NOT_EXPOSED`: a Python scheme is already a dict or JSON text.

#### Rejected alternatives

- **A JSON parser, to merge into a hand-written document.** Not needed to write a document, and it is the part that costs the most to own. Rejected by the owner.
- **Building `paths` from `router::routes()`.** Possible without reflection (method, path, `{name}` parameters), but it would give an operation with no summary, no types and no body, and it ties the writer to the router. It can be added later as a separate function; nothing here prevents it.
- **Python keeps its own writer.** Two implementations of the same rules; the point of the change.
- **Binding every C++ call one by one from `_operation`.** Many calls over the Cython boundary for no gain. `Document.add` takes an operation's fields in one call.

#### Consequences

- Python's document has the same content as before (a test assembles it the old way and compares), in a different key order within an operation. `Server.openapi()` returns a dict parsed from the text, and `/openapi.json` no longer converts a dict per request.
- A Python route's operation now goes through C++'s checks too, so a document that used to be written with an empty `title`, or with a path that does not start with `/`, raises `ValueError` when it is built.
- C++ and Python share the writing rules; they do **not** share how the content is found. Only Python can say "this is what the handler takes".
- Not verified: the Swagger UI in a browser (the tests read the JSON, not the page), MSVC and macOS builds (the code is standard C++17 and was built with GCC 13 only), and the speed of building a document (not measured; it is built once and kept).

### `swagger.hpp`, and one base class for what is mounted on a server

- **Status:** accepted
- **Decided by:** the project owner ("`swagger.hpp` instead of `swagger_ui.hpp`" and "a common interface that swagger, file browser and directory server inherit")
- **Source:** `include/httpp/swagger.hpp`, `include/httpp/server.hpp` (`mountable`, `directory`), `include/httpp/file_browser.hpp`, `src/core/swagger.cpp`, `tests/cpp/test_mountable.cpp`
- **Related:** ADR-0066 (option structs become builders), ADR-0081 (`server` is a shared handle), the part "`swagger` takes the OpenAPI URL in its constructor and the base in `mount`" (`swagger(openapi_url)`)

#### Context

- The header that holds `class swagger` was called `swagger_ui.hpp`. The class is `swagger`, the namespace has no "ui" in it, and the page it serves is the only thing it can mean.
- `directory`, `file_browser` and `swagger` are built the same way: a value you configure with `set_*()` calls, then `server mount(server s, const std::string& base = ...) const`. Nothing let code treat them as one kind of thing.

#### Decision

- `swagger_ui.hpp` becomes `swagger.hpp` (and `src/core/swagger_ui.cpp` becomes `swagger.cpp`, `cy/swagger_ui.pxi` becomes `cy/swagger.pxi`). The names inside stay: `swagger_ui_asset` and `find_swagger_ui_asset` are about the Swagger UI files and say so. The old header is **not** kept as a forwarding file, as with the names the part "`swagger` takes the OpenAPI URL in its constructor and the base in `mount`" removed: the project is before 1.0 and one line fixes an include.
- `httpp::mountable`, declared in `server.hpp` just before `directory`, has `virtual server mount(server s, const std::string& base) const = 0` and a virtual destructor. `directory`, `file_browser` and `swagger` derive from it publicly and mark their `mount` as `override`.
- The base's `mount` has **no default for `base`**: the defaults differ (`"/docs"` for swagger, `"/"` for the others). Each class keeps its own default in its own declaration, so `swagger(url).mount(s)` works as before and a call through the base names the base.
- The base has protected constructors and copy operations. It cannot be made or sliced on its own, and the three stay ordinary copyable values.

#### Rejected alternatives

- **A template or concept instead of a base class.** Cheaper at run time, but code could not keep a list of different parts (`std::vector<std::unique_ptr<mountable>>`) or take one through a function that is not a template.
- **A `server::mount(const mountable&, base)` function.** Not added: nothing needs it yet, and every public function of C++ is a parity decision (ADR-0016). It is one line to add when something does.
- **A forwarding `swagger_ui.hpp`.** See above.
- **Binding `mountable` in Python.** Its only member is not exported (`HTTPP_API` is on the three classes' own `mount`), so the parity test does not see it, and the Python classes (`Directory`, `FileBrowser`, `Swagger`) are unchanged.

#### Consequences

- Code that includes `httpp/swagger_ui.hpp` stops compiling until the include is changed.
- The three classes now have a vtable. Their size grows by one pointer; a call through the base is virtual, a call on the derived type is resolved by the compiler as before.
- `mountable` is not used by the library itself yet; only the tests use it as a base (`tests/cpp/test_mountable.cpp`).
- The parity test finds a class with a base clause (`class directory : public mountable {`).
- **The classes are exported as a whole** (`class HTTPP_API swagger : public mountable`, and the same for `mountable`, `directory` and `file_browser`), the way `logging::handler` is, with MSVC's C4251 silenced around them. A first version exported only the members, as the rest of the library does, and failed to link on Linux (`undefined reference to vtable for httpp::swagger`, and the same inside the Python extension): the copy and the destructor that a caller compiles inline need the vtable, and with hidden visibility it is only exported with the class.
- **Their members carry `HTTPP_MEMBER`, which expands to nothing.** The second version kept `HTTPP_API` on the members as well as on the class, and the Windows build of the CI failed from the commit that introduced it (the builds of the commits before it passed). The compiler log is readable by the repository admin only, so the exact message was not seen; the likely cause is MSVC's refusal of `dllexport` on a member of a `dllexport` class (C2487), and no class exported as a whole in `logging.hpp` repeats the macro on its members. `HTTPP_MEMBER` keeps the public names findable by `tests/python/test_cpp_python_parity.py` without exporting anything twice.
- Not verified: Windows. The class-level export follows the existing `logging::handler` pattern, but it was built with GCC 13 on Linux only; check MSVC before a release.
