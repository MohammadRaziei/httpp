// server::set_compression: gzip for text answers when the client accepts it. The client decodes by itself and
// leaves Content-Encoding and Content-Length as received, which is what these tests read.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <stdexcept>
#include <string>

using testutil::local_server;

namespace {
std::string text(std::size_t n) {
    std::string s;
    while (s.size() < n) s += "The quick brown fox jumps over the lazy dog. ";
    s.resize(n);
    return s;
}
const std::string big = text(20000);

void routes(httpp::server& s) {
    s.get("/text", [](const httpp::request&, httpp::response& r) { r.headers.emplace_back("Content-Type", "text/plain"); r.body = big; });
    s.get("/json", [](const httpp::request&, httpp::response& r) { r.headers.emplace_back("Content-Type", "application/json; charset=utf-8"); r.body = big; });
    s.get("/small", [](const httpp::request&, httpp::response& r) { r.headers.emplace_back("Content-Type", "text/plain"); r.body = "tiny"; });
    s.get("/png", [](const httpp::request&, httpp::response& r) { r.headers.emplace_back("Content-Type", "image/png"); r.body = big; });
    s.get("/already", [](const httpp::request&, httpp::response& r) {
        r.headers.emplace_back("Content-Type", "text/plain");
        r.headers.emplace_back("Content-Encoding", "identity");
        r.body = big;
    });
    s.get_async("/async", [](const httpp::request&, httpp::responder rp) {
        httpp::response r;
        r.headers.emplace_back("Content-Type", "text/html");
        r.body = big;
        rp.send(r);
    });
}
long long length_of(const httpp::response& r) { return std::stoll(r.header("Content-Length")); }
} // namespace

UTEST(httpp_server_compression, a_big_text_answer_is_gzipped_when_the_client_accepts_it) {
    local_server srv([](httpp::server& s) { s.set_compression(1024); routes(s); });
    for (const char* path : {"/text", "/json", "/async"}) {
        const auto r = httpp::client::fetch(srv.url(path));
        ASSERT_EQ(200, r.status);
        ASSERT_STREQ("gzip", r.header("Content-Encoding").c_str());
        ASSERT_NE(std::string::npos, r.header("Vary").find("Accept-Encoding"));
        ASSERT_TRUE(r.body == big);                    // decoded by the client
        ASSERT_LT(length_of(r), 2000);                 // on the wire it is much smaller
    }
}

UTEST(httpp_server_compression, it_is_off_until_it_is_asked_for) {
    local_server srv([](httpp::server& s) { routes(s); });
    const auto r = httpp::client::fetch(srv.url("/text"));
    ASSERT_STREQ("", r.header("Content-Encoding").c_str());
    ASSERT_EQ(static_cast<long long>(big.size()), length_of(r));
}

UTEST(httpp_server_compression, small_bodies_and_other_content_types_are_left_alone) {
    local_server srv([](httpp::server& s) { s.set_compression(1024); routes(s); });
    for (const char* path : {"/small", "/png", "/already"}) {
        const auto r = httpp::client::fetch(srv.url(path));
        ASSERT_EQ(200, r.status);
        ASSERT_STRNE("gzip", r.header("Content-Encoding").c_str());
    }
    ASSERT_STREQ("tiny", httpp::client::fetch(srv.url("/small")).body.c_str());
}

UTEST(httpp_server_compression, a_client_that_does_not_accept_gzip_gets_the_plain_body_and_a_vary_header) {
    local_server srv([](httpp::server& s) { s.set_compression(1024); routes(s); });
    for (const char* accept : {"identity", "gzip;q=0", "deflate", ""}) {
        httpp::client c(srv.url());
        c.set_header("Accept-Encoding", accept);
        const auto r = c.get("/text");
        ASSERT_EQ(200, r.status);
        ASSERT_STREQ("", r.header("Content-Encoding").c_str());
        ASSERT_EQ(static_cast<long long>(big.size()), length_of(r));
        ASSERT_NE(std::string::npos, r.header("Vary").find("Accept-Encoding")); // caches must keep both versions apart
    }
}

UTEST(httpp_server_compression, the_level_is_checked) {
    httpp::server s;
    bool threw = false;
    try { s.set_compression(1024, 10); } catch (const std::invalid_argument&) { threw = true; }
    ASSERT_TRUE(threw);
    threw = false;
    try { s.set_compression(1024, 0); } catch (const std::invalid_argument&) { threw = true; }
    ASSERT_TRUE(threw);
    s.set_compression(0); // 0 turns it off
}
