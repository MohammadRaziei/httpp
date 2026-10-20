#pragma once

// Internal. Adler-32 (zlib) and CRC-32 (gzip) for the inflate / deflate wrappers.

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace httpp::detail {

inline std::uint32_t adler32(const unsigned char* p, std::size_t n) {
    std::uint32_t a = 1, b = 0;
    while (n > 0) {
        const std::size_t k = std::min<std::size_t>(n, 5552);  // largest run that cannot overflow
        for (std::size_t i = 0; i < k; ++i) {
            a += p[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        p += k;
        n -= k;
    }
    return (b << 16) | a;
}

inline std::uint32_t crc32(const unsigned char* p, std::size_t n) {
    struct table_t {
        std::uint32_t v[256];
    };
    static const table_t table = [] {
        table_t t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t.v[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) c = table.v[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

}  // namespace httpp::detail
