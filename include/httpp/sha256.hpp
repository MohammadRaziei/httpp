#pragma once

#include "httpp/export.hpp"

#include <cstddef>
#include <string>

namespace httpp {

// SHA-256 of `size` bytes, as 64 lowercase hex characters.
// ponytail: uses Mbed TLS's hash (already linked for HTTPS), portable code only. Upgrade path if hashing
// 100 MB+ files proves slow: the SHA-NI (x86) / ARMv8 crypto routines, picked at run time, ~4x faster.
HTTPP_API std::string sha256_hex(const void* data, std::size_t size);

// Same, for a whole file, read in chunks (memory use does not depend on the file size).
// Throws std::runtime_error if the file cannot be opened or read.
HTTPP_API std::string sha256_file(const std::string& path);

// Parses the text of a checksum file: one line, `<64 hex digits>`, optionally followed by whitespace, an optional
// `*` and the file name, as `sha256sum` writes it. If a name is there it must equal `file_name`. Returns the digest
// in lowercase. Anything else (several lines, a short line, an HTML error page, ...) throws std::runtime_error.
HTTPP_API std::string parse_sha256_checksum(const std::string& text, const std::string& file_name);

} // namespace httpp
