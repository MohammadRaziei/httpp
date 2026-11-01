# ADR-0031: Files compiled into the library can be gzip-compressed (`embed_files(... COMPRESS)`)

- **Status:** accepted
- **Source:** `cmake/EmbedFile.cmake`, `tests/cmake/embed_compress/`, `tests/CMakeLists.txt`

## Context

`embed_files` (ADR-0008) writes every byte of a data file into a generated header. The Swagger UI bundle is 1.5 MB, and the planned
file browser UI will be embedded the same way. Text assets compress to about a quarter of their size (measured with `gzip -9`:
`swagger-ui-bundle.js` 1587262 to 430288 bytes, `swagger-ui.css` 186154 to 26542, `swagger-api-icon.svg` 2742 to 1336), and a
browser can use the gzip bytes as they are, so a server could send them with no work per request.

## Decision

- `embed_files(... COMPRESS FILES ...)`: the files of that call are gzip-compressed at CMake configure time and the gzip bytes are embedded.
  The call declares two more constants per file, `<name>_raw_size` and `<name>_is_gzip`. Code that uses such a call must check `_is_gzip`.
- `-DHTTPP_EMBED_COMPRESS=OFF` (default ON) turns compression off for every `COMPRESS` call and embeds the original bytes, with
  `_is_gzip = false`. A call without `COMPRESS` is unchanged: its generated header is byte-identical to before (checked on a Swagger-style call).
- The gzip is made by CMake itself (`file(ARCHIVE_CREATE ... FORMAT raw COMPRESSION GZip COMPRESSION_LEVEL 9)`): no Python and nothing from the system.
  CMake writes the current time into the 4 MTIME bytes of the gzip header, so two builds gave different bytes (measured). They are set to zero
  after compression, and the same file gives the same bytes on every build (checked). `MTIME 0` of `ARCHIVE_CREATE` does not touch the gzip header (tried).
- The bytes are embedded as the existing `0xNN` array, not as base64: base64 is 33 percent bigger and needs a decode step at run time.
- `embed_files` now rejects arguments it does not know (before, a misspelled keyword was ignored silently).

## Rejected alternatives

- base64 plus zlib at run time: bigger, and every process would inflate or decode before it can answer.
- Python to compress at build time: the C++ only build (`-DHTTPP_BUILD_PYTHON=OFF`) would need Python.
- Committing `.gz` files: a second copy of each asset that can drift from its source.
- zip: a directory and headers around the stream, and a browser cannot use it.

## Consequences

- CMake's zlib packs about 1 percent worse than `gzip -9` (26893 against 26542 bytes on `swagger-ui.css`). Accepted.
- Swagger UI is the first user (the owner asked for every embedded `.js` and `.css` to move to it). `swagger_ui_asset` is public, so it changed:
  `data` is now the stored bytes (a gzip stream when the new field `gzip` is true, not the file) and `raw_size` is the size of the file. The Python helper
  `_swagger_ui_asset` returns `(stored bytes, content type, is_gzip)`; nothing in the repo calls it. `libhttpp_core.so` went from 4806064 to 3487152 bytes
  (Release, GCC, same tree, `-DHTTPP_EMBED_COMPRESS=OFF` against ON).
- `mount_swagger_ui` answers a client that accepts gzip (`gzip` or `x-gzip` with a quality above 0) with the stored bytes and `Content-Encoding: gzip`,
  and any other client with the file inflated (the vendored inflate, ADR-0019) on every request, with no cache. Both answers carry `Vary: Accept-Encoding`
  when the bytes are stored compressed. `set_compression()` leaves such a body alone (tested). `*` is not read as gzip.
- The file browser UI (ADR-0032) will use the same call.
- Tested by `embed_compress_ON` and `embed_compress_OFF` (ctest): flag and sizes, gzip magic, zero MTIME, and the CRC-32 and size in the gzip
  trailer against the original file; and by `httpp_swagger_ui*` (C++, 12 tests, run with ON and with OFF): the stored bytes inflate to the vendored files,
  the negotiation above, and no second compression. The deflate data itself was checked by hand once with Python's `gzip.decompress` (equal), not by a test.
- Not verified: Windows and macOS runs, other CMake versions than 3.28.3, MSVC with the byte array of a gzip file.

## Related

ADR-0008, ADR-0019
