# ADR-0008: Swagger UI is compiled into the library (no CDN, no data files)

- **Status:** accepted
- **Source:** `cmake/EmbedFile.cmake`, `src/core/swagger_ui.cpp`, `src/third_party/swagger-ui/VENDORED.md`

## Context

`/docs` must work offline (no CDN), and the installed package must not contain a folder of `.js/.css/.png` files.

## Decision

- Swagger UI 5.33.x is **vendored** in `src/third_party/swagger-ui/` (Apache-2.0).
- `embed_files(...)` turns each file into **one** generated header at configure time: `inline constexpr unsigned char n[]`
  in `.rodata`, one literal per **32 bytes**, each byte written as `\xNN` (the JS bundle is one 1.5 MB line and the PNG contains NUL bytes).
  The header is rewritten only when the input changes (`configure_file`), and `CMAKE_CONFIGURE_DEPENDS` tracks source changes.
- C++: `httpp::find_swagger_ui_asset(name)` and `httpp::mount_swagger_ui(server&, docs_path, openapi_url, title)`.
  Python: `Server.mount_swagger_ui` (the page and the three files are served from C++ without the GIL).

## Rejected alternatives

A CDN (not offline), a data folder next to the wheel, `inline const std::string` (every process copied 1.6 MB onto the heap).
ReDoc was not added (a second bundle of about 1 MB).

## Consequences

- Each request copies the body (`ponytail:`; upgrade path: a static `string_view` body inside `response`).
- A `-Woverlength-strings` warning for the multi-megabyte literals (not an error; suggestion: make `HTTPP_EMBED_AS_ARRAY` the default).
- `/docs` has only been seen in Chromium.
- Evaluated and **not decided:** a lighter replacement. Bundle file size (raw / gzip -9): Swagger 1550 / 420 KB
  (+ CSS 181 KB); RapiDoc 524 / 135 KB; Stoplight Elements 2035 / 599 KB; Redoc 2236 / 634 KB; Scalar 4512 / 1303 KB.
  RapiDoc's compatibility with the current `openapi.json` and its look have not been tried.

## Related

ADR-0007
