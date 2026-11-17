# ADR-0087: one OpenAPI document writer in C++, used by C++ and by Python

- **Status:** accepted
- **Decided by:** the project owner ("implement what needs no JSON parser and no hack, in C++ and in Python")
- **Source:** `include/httpp/openapi.hpp`, `src/core/openapi.cpp`, `src/bindings/python/cy/openapi.pxi`, `src/bindings/python/httpp/openapi.py`, `_schema.py`, `tests/cpp/test_openapi.cpp`, `tests/python/test_openapi_document.py`
- **Related:** ADR-0007 (the Python document is built from the request plan; unchanged), ADR-0016 (C++ was not required to generate OpenAPI), ADR-0017 (what is bound), ADR-0082 (`swagger` serves the page, not the document)

## Context

ADR-0016 says C++ needs no OpenAPI generator, and that stays true for the part that needs reflection: reading the arguments of a handler. But a C++ user who wants the Swagger page still had to write the document as a raw string, with their own escaping, and the rules (which security names exist, how paths and methods group, what `security: []` means) lived only in Python's `_schema.py`.

## Decision

- `httpp::openapi::document` (header `httpp/openapi.hpp`, not part of `httpp.h`) is a **writer**: C++ calls describe the API and `to_json()` returns compact JSON text. It checks what it can check without parsing: security names against the declared schemes, duplicate operations and operation ids, parameter places, a required path parameter, reserved keys given to `raw()`, valid UTF-8.
- It has **no JSON parser**. Anything bigger than a few fields (a schema, a parameter object, a request body, a response `content`) is JSON text that the caller supplies and the library puts in as it is. The library refuses only text that is blank or not UTF-8. That the text is valid JSON is the caller's promise, written in the header and in the docs.
- It reads **no routes**: the document is described apart from the handlers. The docs say plainly that it can drift. `server` is not changed.
- `httpp::openapi::{basic, bearer, api_key, raw_scheme}` build scheme objects; `any_of({...})` and `operation::no_security()` cover the common requirements.
- Python uses the same class: `httpp.openapi.Document`. `_schema.build_openapi_json` builds the operations as before (parameters, bodies, responses and model schemas still come from type hints, in Python: ADR-0017 rule 4) and hands them to `Document`, which assembles the paths, validates the security names and writes the text. `Server.openapi()` returns `json.loads` of that text and `/openapi.json` serves the text itself, written once and kept.
- `httpp.security.basic/bearer/api_key` keep returning dicts (their shape is tested and public); the C++ factories of the same names are the C++ face of them. `raw_scheme` is `NOT_EXPOSED`: a Python scheme is already a dict or JSON text.

## Rejected alternatives

- **A JSON parser, to merge into a hand-written document.** Not needed to write a document, and it is the part that costs the most to own. Rejected by the owner.
- **Building `paths` from `router::routes()`.** Possible without reflection (method, path, `{name}` parameters), but it would give an operation with no summary, no types and no body, and it ties the writer to the router. It can be added later as a separate function; nothing here prevents it.
- **Python keeps its own writer.** Two implementations of the same rules; the point of the change.
- **Binding every C++ call one by one from `_operation`.** Many calls over the Cython boundary for no gain. `Document.add` takes an operation's fields in one call.

## Consequences

- Python's document has the same content as before (a test assembles it the old way and compares), in a different key order within an operation. `Server.openapi()` returns a dict parsed from the text, and `/openapi.json` no longer converts a dict per request.
- A Python route's operation now goes through C++'s checks too, so a document that used to be written with an empty `title`, or with a path that does not start with `/`, raises `ValueError` when it is built.
- C++ and Python share the writing rules; they do **not** share how the content is found. Only Python can say "this is what the handler takes".
- Not verified: the Swagger UI in a browser (the tests read the JSON, not the page), MSVC and macOS builds (the code is standard C++17 and was built with GCC 13 only), and the speed of building a document (not measured; it is built once and kept).
