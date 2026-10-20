#pragma once

// Internal. Compression for HTTP bodies (Content-Encoding: gzip / deflate, request bodies, server responses)
// on top of the vendored stb compressor (src/third_party/stb_deflate). Zero system dependencies: no zlib.
// The output is standard: anything zlib, gzip or a browser reads. See inflate.hpp for the reverse direction.

#include <cstddef>
#include <string>

#include "internal/inflate.hpp"

namespace httpp::detail {

// `level` uses zlib's numbering: 0 stores (no compression), 1 is fastest, 9 is smallest, -1 means 6.
// `format` is zlib, gzip or raw; inflate_format::deflate is the HTTP name for zlib and is treated as such.
// Throws std::invalid_argument (level outside -1..9, null data with a size, input over 2 GiB), std::bad_alloc.
std::string deflate(const void* data, std::size_t size, inflate_format format, int level = -1);

}  // namespace httpp::detail
