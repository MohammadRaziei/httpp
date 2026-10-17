# swagger-ui

`swagger-ui-dist` **5.33.2**, Apache-2.0 (`LICENSE`, `NOTICE`, `swagger-ui-bundle.js.LICENSE.txt` are upstream's), from
`https://registry.npmjs.org/swagger-ui-dist`. Only the two files httpp serves at `/docs` are kept; source maps and the
other presets are left out.

These files are **not shipped**: `cmake/EmbedFile.cmake` turns them into a C++ header at CMake configure time and
`src/core/swagger_ui.cpp` compiles them into `libhttpp`, so nothing has to be found on disk at run time. To bump the
version: `npm pack swagger-ui-dist@<version>`, copy the same two files (+ the licence files) over these, update the
version above, re-run CMake.
