#include "internal/deflate.hpp"

#include <climits>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>

#include "internal/checksums.hpp"
#include "stb_deflate/stb_deflate.h"

namespace httpp::detail {
namespace {

struct free_deleter {
    void operator()(unsigned char* p) const { std::free(p); }
};

void put_be32(std::string& s, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) s.push_back(static_cast<char>(v >> shift));
}
void put_le32(std::string& s, std::uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8) s.push_back(static_cast<char>(v >> shift));
}

// Level 0: stored blocks of up to 65535 bytes (also one empty final block for empty input).
std::string raw_store(const unsigned char* data, std::size_t size) {
    std::string out;
    out.reserve(size + 5 * (size / 65535 + 1));
    std::size_t pos = 0;
    do {
        const std::size_t n = std::min<std::size_t>(size - pos, 65535);
        out.push_back(pos + n == size ? 1 : 0);  // BFINAL, BTYPE = 00
        out.push_back(static_cast<char>(n));
        out.push_back(static_cast<char>(n >> 8));
        out.push_back(static_cast<char>(~n));
        out.push_back(static_cast<char>(~n >> 8));
        out.append(reinterpret_cast<const char*>(data) + pos, n);
        pos += n;
    } while (pos < size);
    return out;
}

// Bare deflate data. stb writes a zlib stream (2-byte header, data, 4-byte Adler-32): take the middle.
std::string raw_deflate(const unsigned char* data, std::size_t size, int level) {
    if (level == 0) return raw_store(data, size);
    if (size == 0) return std::string("\x03\x00", 2);  // one empty fixed-Huffman final block
    // stb's effort table has 11 entries: 1..8 map to themselves and 9 to its strongest (10).
    const int quality = level == 9 ? 10 : level;
    int n = 0;
    std::unique_ptr<unsigned char, free_deleter> z(
        stbi_zlib_compress(const_cast<unsigned char*>(data), static_cast<int>(size), &n, quality));
    if (!z || n < 6) throw std::bad_alloc();
    return std::string(reinterpret_cast<const char*>(z.get()) + 2, static_cast<std::size_t>(n) - 6);
}

}  // namespace

std::string deflate(const void* data, std::size_t size, inflate_format format, int level) {
    if (level < -1 || level > 9) throw std::invalid_argument("level must be in -1..9");
    if (level == -1) level = 6;
    if (!data && size != 0) throw std::invalid_argument("data is null");
    if (size > static_cast<std::size_t>(INT_MAX)) throw std::invalid_argument("input larger than 2 GiB");
    const auto* in = static_cast<const unsigned char*>(data);

    std::string body = raw_deflate(in, size, level);
    switch (format) {
        case inflate_format::raw: return body;
        case inflate_format::zlib:
        case inflate_format::deflate: {
            std::string out;
            out.reserve(body.size() + 6);
            out.push_back(0x78);
            out.push_back(level == 0 ? 0x01 : level < 6 ? 0x5E : level == 6 ? static_cast<char>(0x9C) : static_cast<char>(0xDA));
            out += body;
            put_be32(out, adler32(in, size));
            return out;
        }
        case inflate_format::gzip: {
            std::string out;
            out.reserve(body.size() + 18);
            // magic, deflate, no flags, no mtime, XFL (2 best, 4 fastest, 0 otherwise), OS 255 (unknown)
            const char xfl = level == 9 ? 2 : level == 1 ? 4 : 0;
            out.append("\x1f\x8b\x08\x00\x00\x00\x00\x00", 8);
            out.push_back(xfl);
            out.push_back(static_cast<char>(0xFF));
            out += body;
            put_le32(out, crc32(in, size));
            put_le32(out, static_cast<std::uint32_t>(size));
            return out;
        }
    }
    throw std::invalid_argument("unknown format");
}

}  // namespace httpp::detail
