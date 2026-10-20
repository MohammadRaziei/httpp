#pragma once

// Internal. Undo a response's Content-Encoding in place (gzip, x-gzip, deflate, and chains of them), so that
// the client hands out the real body. Used by both client engines (blocking cpp-httplib and the async one).
//
// Headers are left as received (like requests and httpx): Content-Encoding and Content-Length still describe
// what was on the wire. An encoding we do not advertise (br, zstd, compress) leaves the body untouched.

#include <cstddef>

#include "httpp/client.hpp"

namespace httpp::detail {

// The value of Accept-Encoding the client sends when the caller did not set one.
inline constexpr const char* default_accept_encoding = "gzip, deflate";

// No-op for an error response, an empty body, or no/unknown/identity Content-Encoding.
// Decoded output larger than `max_bytes` (0 = no limit) gives error_kind::too_large (status 413); corrupt data
// gives error_kind::other (status 0). On either, the body is cleared.
void decode_content(response& res, std::size_t max_bytes);

} // namespace httpp::detail
