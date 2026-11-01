// Checks what embed_files(... COMPRESS) generated. Exit code 0 = all good.
#include "data.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        failures++;
    }
}

std::uint32_t crc32(const std::string& s) { // bitwise, slow and short: the sample is a few KB
    std::uint32_t c = 0xffffffffu;
    for (unsigned char b : s) {
        c ^= b;
        for (int i = 0; i < 8; i++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

std::uint32_t le32(const unsigned char* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

} // namespace

int main() {
    std::ifstream in(SAMPLE_PATH, std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check(!raw.empty(), "the sample file is readable");

    namespace e = test_embedded;
    check(e::sample_raw_size == raw.size(), "raw_size is the size of the original file");
    check(e::sample_is_gzip == (EXPECT_GZIP != 0), "is_gzip follows HTTPP_EMBED_COMPRESS");
    check(sizeof(e::sample) == e::sample_size, "size is the size of the array");

    if (!e::sample_is_gzip) {
        check(e::sample_size == raw.size(), "uncompressed: the stored size is the raw size");
        check(std::memcmp(e::sample, raw.data(), raw.size()) == 0, "uncompressed: the bytes are the file's bytes");
    } else {
        const unsigned char* g = e::sample;
        check(e::sample_size > 18 && e::sample_size < raw.size(), "gzip: smaller than the original");
        check(g[0] == 0x1f && g[1] == 0x8b && g[2] == 8, "gzip: magic and the deflate method");
        check(g[4] == 0 && g[5] == 0 && g[6] == 0 && g[7] == 0, "gzip: MTIME is zero (the same bytes on every build)");
        if (e::sample_size > 18) { // the trailer: CRC-32 and size of the original, little endian
            check(le32(g + e::sample_size - 8) == crc32(raw), "gzip: the trailer CRC-32 matches the original");
            check(le32(g + e::sample_size - 4) == raw.size(), "gzip: the trailer size matches the original");
        }
    }
    if (failures == 0) std::puts("embed_compress: all checks passed");
    return failures == 0 ? 0 : 1;
}
