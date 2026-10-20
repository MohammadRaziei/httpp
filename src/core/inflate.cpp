#include "internal/inflate.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>

#include "internal/checksums.hpp"
#include "stb_inflate/stb_inflate.h"

namespace httpp::detail {
namespace {

constexpr std::size_t int_max = static_cast<std::size_t>(INT_MAX);

struct free_deleter {
    void operator()(char* p) const { std::free(p); }
};
using malloc_buffer = std::unique_ptr<char, free_deleter>;

std::uint32_t be32(const unsigned char* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint32_t le32(const unsigned char* p) {
    return (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[1]) << 8) | p[0];
}

// Inflate bare deflate data (or a zlib stream when with_header). stb grows its buffer by doubling from
// `initial`; choose `initial` so that after k doublings it lands exactly on `ceiling` >= limit (never up
// to twice past it), and refuse any growth beyond the ceiling.
std::string run(const unsigned char* in, std::size_t size, bool with_header, std::size_t limit) {
    const std::size_t guess = std::min(limit, std::max<std::size_t>(size * 4, std::size_t(1) << 16));
    unsigned k = 0;
    while (((limit + (std::size_t(1) << k) - 1) >> k) > guess) ++k;
    const std::size_t initial = std::max<std::size_t>((limit + (std::size_t(1) << k) - 1) >> k, 1);

    httpp_stb_inflate::ceiling = initial << k;
    httpp_stb_inflate::refused = false;
    httpp_stb_inflate::reason = nullptr;
    int n = 0;
    malloc_buffer out(stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char*>(in),
                                                                   static_cast<int>(size), static_cast<int>(initial),
                                                                   &n, with_header ? 1 : 0));
    const bool refused = httpp_stb_inflate::refused;
    const char* why = httpp_stb_inflate::reason;
    httpp_stb_inflate::ceiling = 0;

    if (!out) {
        if (refused) throw inflate_limit_error("inflated data exceeds " + std::to_string(limit) + " bytes");
        if (why && std::string(why) == "Out of memory") throw std::bad_alloc();
        throw inflate_error("corrupt or truncated deflate data");
    }
    if (n < 0 || static_cast<std::size_t>(n) > limit)  // fit the rounded-up buffer, but not the limit
        throw inflate_limit_error("inflated data exceeds " + std::to_string(limit) + " bytes");
    return std::string(out.get(), static_cast<std::size_t>(n));
}

std::string inflate_zlib(const unsigned char* in, std::size_t size, std::size_t limit) {
    if (size < 6) throw inflate_error("zlib: stream too short");
    std::string out = run(in, size, true, limit);
    // stb does not read the Adler-32 trailer. Checking it also proves the input ends where the stream ends.
    if (be32(in + size - 4) != adler32(reinterpret_cast<const unsigned char*>(out.data()), out.size()))
        throw inflate_error("zlib: checksum mismatch, or the stream is truncated or followed by other data");
    return out;
}

std::string inflate_gzip(const unsigned char* in, std::size_t size, std::size_t limit) {
    // Fixed 10-byte header, optional fields, deflate data, then CRC-32 and ISIZE (8 bytes).
    if (size < 18 || in[0] != 0x1f || in[1] != 0x8b) throw inflate_error("gzip: bad header");
    if (in[2] != 8) throw inflate_error("gzip: unknown compression method");
    const unsigned flags = in[3];
    if (flags & 0xE0) throw inflate_error("gzip: reserved flags set");
    std::size_t pos = 10;
    const std::size_t end = size - 8;
    if (flags & 4) {  // FEXTRA
        if (pos + 2 > end) throw inflate_error("gzip: truncated header");
        pos += 2 + (std::size_t(in[pos]) | (std::size_t(in[pos + 1]) << 8));
        if (pos > end) throw inflate_error("gzip: truncated header");
    }
    for (const unsigned bit : {8u, 16u}) {  // FNAME, FCOMMENT: zero-terminated strings
        if (!(flags & bit)) continue;
        while (pos < end && in[pos] != 0) ++pos;
        if (pos >= end) throw inflate_error("gzip: truncated header");
        ++pos;
    }
    if (flags & 2) pos += 2;  // FHCRC
    if (pos >= end) throw inflate_error("gzip: truncated header");

    std::string out = run(in + pos, end - pos, false, limit);
    if (le32(in + size - 8) != crc32(reinterpret_cast<const unsigned char*>(out.data()), out.size()) ||
        le32(in + size - 4) != static_cast<std::uint32_t>(out.size()))
        throw inflate_error("gzip: checksum mismatch, or the stream is truncated or followed by other data");
    return out;
}

bool looks_like_zlib(const unsigned char* in, std::size_t size) {
    return size >= 2 && (in[0] & 0x0F) == 8 && ((unsigned(in[0]) << 8) | in[1]) % 31 == 0;
}

}  // namespace

std::string inflate(const void* data, std::size_t size, inflate_format format, std::size_t max_bytes) {
    if (!data && size != 0) throw std::invalid_argument("data is null");
    if (size > int_max) throw std::invalid_argument("input larger than 2 GiB");
    if (size == 0) throw inflate_error("empty input");
    const std::size_t limit = std::min(max_bytes, int_max);
    const auto* in = static_cast<const unsigned char*>(data);
    switch (format) {
        case inflate_format::zlib: return inflate_zlib(in, size, limit);
        case inflate_format::gzip: return inflate_gzip(in, size, limit);
        case inflate_format::raw: return run(in, size, false, limit);
        case inflate_format::deflate:
            return looks_like_zlib(in, size) ? inflate_zlib(in, size, limit) : run(in, size, false, limit);
    }
    throw std::invalid_argument("unknown format");
}

}  // namespace httpp::detail
