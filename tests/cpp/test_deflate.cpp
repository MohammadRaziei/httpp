// httpp::detail::deflate: output must be standard (checked against Python's zlib/gzip in the fuzz script),
// and must round-trip through our own inflate here.
#include "utest/utest.h"
#include "internal/deflate.hpp"
#include "internal/inflate.hpp"

#include <string>

using httpp::detail::deflate;
using httpp::detail::inflate;
using httpp::detail::inflate_format;

namespace {
const std::size_t big = std::size_t(1) << 26;

std::string sample(std::size_t n) {
    std::string s;
    unsigned x = 12345;
    while (s.size() < n) {
        x = x * 1103515245u + 12345u;
        s += (x >> 16) % 5 == 0 ? std::string("random-") + char('a' + (x >> 8) % 26) : std::string("The quick brown fox. ");
    }
    s.resize(n);
    return s;
}
template <class E, class F> bool throws(F f) {
    try { f(); } catch (const E&) { return true; } catch (...) { return false; }
    return false;
}
} // namespace

UTEST(httpp_deflate, every_format_and_level_round_trips) {
    const std::string text = sample(100000);
    for (const auto f : {inflate_format::zlib, inflate_format::gzip, inflate_format::raw, inflate_format::deflate}) {
        for (int level = -1; level <= 9; ++level) {
            const std::string z = deflate(text.data(), text.size(), f, level);
            EXPECT_TRUE(inflate(z.data(), z.size(), f, big) == text);
        }
    }
}

UTEST(httpp_deflate, empty_and_tiny_inputs) {
    for (const auto f : {inflate_format::zlib, inflate_format::gzip, inflate_format::raw}) {
        for (int level : {0, 1, 6, 9}) {
            for (const char* s : {"", "a", "ab", "aaaaaaaaaaaaaaaaaaaaaaaa"}) {
                const std::string in(s);
                const std::string z = deflate(in.data(), in.size(), f, level);
                EXPECT_TRUE(inflate(z.data(), z.size(), f, big) == in);
            }
        }
    }
}

UTEST(httpp_deflate, text_gets_smaller_and_level_0_is_stored) {
    const std::string text = sample(100000);
    const std::string z = deflate(text.data(), text.size(), inflate_format::gzip, 6);
    EXPECT_TRUE(z.size() < text.size() / 4);
    const std::string stored = deflate(text.data(), text.size(), inflate_format::zlib, 0);
    EXPECT_TRUE(stored.size() > text.size());  // a few bytes per 64 KiB larger than the input
    EXPECT_TRUE(stored.size() < text.size() + 100);
}

UTEST(httpp_deflate, a_larger_than_one_block_input_works) {
    const std::string big_in = sample(3u << 20);  // several 64 Ki-token blocks and stored chunks
    const std::string z = deflate(big_in.data(), big_in.size(), inflate_format::gzip, 9);
    EXPECT_TRUE(inflate(z.data(), z.size(), inflate_format::gzip, big) == big_in);
}

UTEST(httpp_deflate, bad_arguments_throw) {
    EXPECT_TRUE(throws<std::invalid_argument>([] { deflate("a", 1, inflate_format::zlib, 10); }));
    EXPECT_TRUE(throws<std::invalid_argument>([] { deflate("a", 1, inflate_format::zlib, -2); }));
    EXPECT_TRUE(throws<std::invalid_argument>([] { deflate(nullptr, 3, inflate_format::gzip, 6); }));
}
