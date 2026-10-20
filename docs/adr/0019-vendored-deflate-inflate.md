# ADR-0019: gzip and deflate on vendored stb code, no system zlib

- **Status:** accepted
- **Date:** 2026-10-20
- **Decided by:** the project owner (asked for zlib in the client and the server, and pointed at the code in libstb); the wiring is mine
- **Source:** `src/third_party/stb_inflate/`, `src/third_party/stb_deflate/`, `src/core/inflate.cpp`, `src/core/deflate.cpp`,
  `src/core/content_decode.cpp`, `tests/cpp/test_inflate.cpp`, `test_deflate.cpp`, `test_client_encoding.cpp`

## Context

The client did not undo `Content-Encoding: gzip` and never asked for it. cpp-httplib can do it only when built against zlib, and
[ADR-0004](0004-zero-system-dependencies.md) rules out a system zlib. The server will want a GZip middleware, which needs a compressor.

## Decision

- Vendor the decoder of stb_image.h (`stb_inflate`) and the compressor of libstb (`stb_deflate`), each **extracted unmodified** from
  its host header (the exact line ranges and a `diff` check are in the `VENDORED.md` next to each). Only the small shim around the
  extracted text is ours. Each header is included by exactly one `.cpp`, because every function is `static`.
- Wrap them in two internal functions, `httpp::detail::inflate` and `deflate` (zlib, gzip, raw; levels 0 to 9). Checksums (Adler-32,
  CRC-32) are ours, because stb does not read the trailers. Decoding takes a hard output limit, enforced by refusing buffer growth
  (an exact power-of-two ceiling, so that stb's doubling never writes past what we allowed).
- Client: both engines send `Accept-Encoding: gzip, deflate` unless the caller set one, and undo `gzip`, `x-gzip`, `deflate` (zlib,
  falling back to raw) and chains of them, in one shared function (`decode_content`). cpp-httplib is told not to decode
  (`set_decompress(false)`). Headers are left as received (requests and httpx do the same). `max_response_size` limits the decoded size.
  A coding we do not advertise leaves the body untouched. No new public API: `Accept-Encoding: identity` asks for no compression.

## Rejected alternatives

- **System zlib or a vendored zlib/miniz:** the first breaks ADR-0004; the second is more code than needed, and stb is already vendored.
- **Vendoring all of stb_image.h / stb_image_write.h:** 283 KB and 88 KB (JPEG, PNG, BMP and more) for about 500 and 450 lines of use.
- **Decoding only in the async engine:** the blocking client would stay inconsistent (a gzip body would fail with 415 from cpp-httplib).
- **A public `decompress(false)` option now:** not needed to opt out; add it only if a real use appears.

## Consequences

- Decoding is a pass over the whole body after it was received (not streamed), which matches the buffered client. `download()` streams
  to a file and sends no `Accept-Encoding`, so it saves exactly what the server sends.
- The compressor is not used by any shipped feature yet: the GZip middleware and request-body compression are still to do
  (checklist, "Middleware"). It is tested (round trips, and Python's zlib/gzip reading our output, 3000 cases).
- **Not verified:** speed. stb inflate is simple and not as fast as zlib, and no benchmark has been run. Compression ratio at level 9
  versus zlib was not measured either.
- Decoding a multi-member gzip stream or a stream followed by other bytes is an error (the trailer check); HTTP bodies are one member.

## Related

[ADR-0003](0003-engine-and-third-party.md), [ADR-0004](0004-zero-system-dependencies.md), [ADR-0015](0015-testing-and-ci.md).
