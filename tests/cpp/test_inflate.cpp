// httpp::detail::inflate: zlib, gzip, raw and HTTP "deflate" bodies on the vendored stb inflate.
#include "utest/utest.h"
#include "inflate_fixtures.hpp"
#include "internal/inflate.hpp"

#include <string>
#include <vector>

using httpp::detail::inflate;
using httpp::detail::inflate_error;
using httpp::detail::inflate_format;
using httpp::detail::inflate_limit_error;

namespace {
const std::size_t big = std::size_t(1) << 26;

template <std::size_t N> std::string run(const unsigned char (&a)[N], inflate_format f, std::size_t max = big) {
    return inflate(a, N, f, max);
}
template <class E, class F> bool throws(F f) {
    try { f(); } catch (const E&) { return true; } catch (...) { return false; }
    return false;
}
} // namespace

UTEST(httpp_inflate, zlib_gzip_and_raw_give_the_original) {
    EXPECT_TRUE(run(fx_text_zlib, inflate_format::zlib) == fx_text);
    EXPECT_TRUE(run(fx_text_gzip, inflate_format::gzip) == fx_text);   // header carries a file name
    EXPECT_TRUE(run(fx_text_raw, inflate_format::raw) == fx_text);
}

UTEST(httpp_inflate, stored_and_fixed_huffman_blocks) {
    EXPECT_TRUE(run(fx_hello_stored, inflate_format::zlib) == "hello");
    EXPECT_TRUE(run(fx_fixed_huff, inflate_format::zlib) == "abcabcabcabc");
}

UTEST(httpp_inflate, empty_output_is_fine) {
    EXPECT_TRUE(run(fx_empty_zlib, inflate_format::zlib).empty());
}

UTEST(httpp_inflate, http_deflate_accepts_both_zlib_and_raw) {
    EXPECT_TRUE(run(fx_text_zlib, inflate_format::deflate) == fx_text);
    EXPECT_TRUE(run(fx_text_raw, inflate_format::deflate) == fx_text);
}

UTEST(httpp_inflate, output_is_bounded_zip_bomb) {
    // 1 MiB of zeros in about 1 KiB: a ceiling below that is a limit error, at or above it works
    EXPECT_TRUE(throws<inflate_limit_error>([] { run(fx_zeros_gzip, inflate_format::gzip, 1000); }));
    EXPECT_TRUE(throws<inflate_limit_error>([] { run(fx_zeros_gzip, inflate_format::gzip, (1u << 20) - 1); }));
    EXPECT_TRUE(run(fx_zeros_gzip, inflate_format::gzip, 1u << 20).size() == (1u << 20));
}

UTEST(httpp_inflate, a_limit_that_is_not_a_power_of_two_still_admits_exactly_that_much) {
    EXPECT_TRUE(throws<inflate_limit_error>([] { run(fx_text_zlib, inflate_format::zlib, 2399); }));
    EXPECT_TRUE(run(fx_text_zlib, inflate_format::zlib, 2400) == fx_text);
    EXPECT_TRUE(run(fx_text_zlib, inflate_format::zlib, 2401) == fx_text);
}

UTEST(httpp_inflate, corrupt_truncated_and_trailing_data_throw) {
    std::vector<unsigned char> z(fx_text_zlib, fx_text_zlib + sizeof fx_text_zlib);
    std::vector<unsigned char> g(fx_text_gzip, fx_text_gzip + sizeof fx_text_gzip);

    auto flipped = z; flipped[flipped.size() - 1] ^= 1;  // wrong Adler-32
    EXPECT_TRUE(throws<inflate_error>([&] { inflate(flipped.data(), flipped.size(), inflate_format::zlib, big); }));

    auto cut = z; cut.resize(cut.size() / 2);
    EXPECT_TRUE(throws<inflate_error>([&] { inflate(cut.data(), cut.size(), inflate_format::zlib, big); }));

    auto extra = z; extra.push_back(0);
    EXPECT_TRUE(throws<inflate_error>([&] { inflate(extra.data(), extra.size(), inflate_format::zlib, big); }));

    auto badcrc = g; badcrc[badcrc.size() - 8] ^= 1;
    EXPECT_TRUE(throws<inflate_error>([&] { inflate(badcrc.data(), badcrc.size(), inflate_format::gzip, big); }));

    auto badsize = g; badsize[badsize.size() - 1] ^= 1;
    EXPECT_TRUE(throws<inflate_error>([&] { inflate(badsize.data(), badsize.size(), inflate_format::gzip, big); }));

    auto gcut = g; gcut.resize(gcut.size() - 20);
    EXPECT_TRUE(throws<inflate_error>([&] { inflate(gcut.data(), gcut.size(), inflate_format::gzip, big); }));

    auto notgz = z;  // zlib bytes given to the gzip decoder
    EXPECT_TRUE(throws<inflate_error>([&] { inflate(notgz.data(), notgz.size(), inflate_format::gzip, big); }));
}

UTEST(httpp_inflate, garbage_and_empty_input_throw) {
    const unsigned char junk[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    EXPECT_TRUE(throws<inflate_error>([&] { inflate(junk, sizeof junk, inflate_format::raw, big); }));
    EXPECT_TRUE(throws<inflate_error>([&] { inflate(junk, sizeof junk, inflate_format::deflate, big); }));
    EXPECT_TRUE(throws<inflate_error>([&] { inflate("", 0, inflate_format::gzip, big); }));
    EXPECT_TRUE(throws<std::invalid_argument>([&] { inflate(nullptr, 5, inflate_format::zlib, big); }));
}
