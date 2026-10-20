#pragma once

// Internal. Decompression of HTTP bodies (Content-Encoding: gzip / deflate) and of plain zlib or raw
// deflate data, on top of the vendored stb inflate (src/third_party/stb_inflate). Zero system
// dependencies: no zlib is linked.
//
// The output is always bounded by `max_bytes`: a small stream can expand a thousand-fold (a "zip
// bomb"), so the buffer is never allowed to grow past that limit.

#include <cstddef>
#include <stdexcept>
#include <string>

namespace httpp::detail {

enum class inflate_format {
    zlib,     // RFC 1950: 2-byte header, deflate data, Adler-32 trailer
    gzip,     // RFC 1952: header, deflate data, CRC-32 and size trailer (one member)
    raw,      // RFC 1951: bare deflate data, no header and no checksum
    deflate,  // what HTTP calls "deflate": zlib, falling back to raw when there is no zlib header
};

// The data is corrupt, truncated, or fails its checksum.
struct inflate_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The output would be larger than max_bytes.
struct inflate_limit_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Throws inflate_error, inflate_limit_error, std::bad_alloc, std::invalid_argument (null data with a
// size, or input over 2 GiB).
std::string inflate(const void* data, std::size_t size, inflate_format format, std::size_t max_bytes);

}  // namespace httpp::detail
