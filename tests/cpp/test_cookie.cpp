// httpp::cookie, server_request::cookies / cookie / basic_auth, response::set_cookie (include/httpp/cookie.hpp).
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/cookie.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <stdexcept>
#include <string>

using testutil::local_server;

namespace {
template <class F> bool throws_invalid(F fn) {
    try { fn(); } catch (const std::invalid_argument&) { return true; }
    return false;
}
} // namespace

UTEST(httpp_cookie, the_defaults_are_path_slash_and_http_only) {
    ASSERT_STREQ("sid=abc; Path=/; HttpOnly", httpp::cookie("sid", "abc").to_set_cookie().c_str());
}

UTEST(httpp_cookie, every_attribute_is_written_in_a_fixed_order) {
    auto c = httpp::cookie("sid", "abc").set_path("/app").set_domain("example.com").set_max_age(3600).set_secure(true).set_http_only(false).set_same_site(httpp::same_site::strict);
    ASSERT_STREQ("sid=abc; Path=/app; Domain=example.com; Max-Age=3600; Secure; SameSite=Strict", c.to_set_cookie().c_str());
    ASSERT_STREQ("a=1; Max-Age=0; HttpOnly; SameSite=None", httpp::cookie("a", "1").set_path("").set_max_age(0).set_same_site(httpp::same_site::none).to_set_cookie().c_str());
    ASSERT_STREQ("a=; Path=/; HttpOnly; SameSite=Lax", httpp::cookie("a", "").set_same_site(httpp::same_site::lax).to_set_cookie().c_str());
}

UTEST(httpp_cookie, what_cannot_be_sent_is_refused) {
    for (const char* bad : {"", "a b", "a;b", "a=b", "a\r\nX: 1", "caf\xc3\xa9", "a,b", "a\"b"}) ASSERT_TRUE(throws_invalid([&] { httpp::cookie c(bad, "v"); }));
    for (const char* bad : {"a b", "a;b", "a,b", "a\"b", "a\\b", "a\r\nX: 1", "caf\xc3\xa9"}) ASSERT_TRUE(throws_invalid([&] { httpp::cookie c("n", bad); }));
    httpp::cookie c("n", "v");
    ASSERT_TRUE(throws_invalid([&] { c.set_path("/a;b"); }));
    ASSERT_TRUE(throws_invalid([&] { c.set_domain("a\r\nb"); }));
    ASSERT_STREQ("n=v; Path=/; HttpOnly", c.to_set_cookie().c_str()); // nothing changed
}

UTEST(httpp_cookie, a_cookie_header_is_split_into_pairs) {
    auto pairs = httpp::cookie::parse_cookie_header("a=1; b=2;c=3 ;  d = 4 ; a=9; novalue; =x; e=");
    ASSERT_EQ(6u, pairs.size());
    ASSERT_STREQ("a", pairs[0].first.c_str());
    ASSERT_STREQ("1", pairs[0].second.c_str());
    ASSERT_STREQ("c", pairs[2].first.c_str());
    ASSERT_STREQ("3", pairs[2].second.c_str());
    ASSERT_STREQ("d", pairs[3].first.c_str());
    ASSERT_STREQ("4", pairs[3].second.c_str());
    ASSERT_STREQ("9", pairs[4].second.c_str()); // a repeated name is kept
    ASSERT_STREQ("e", pairs[5].first.c_str());
    ASSERT_STREQ("", pairs[5].second.c_str());
    ASSERT_TRUE(httpp::cookie::parse_cookie_header("").empty());
}

UTEST(httpp_cookie, a_request_reads_its_cookies_and_basic_credentials) {
    local_server srv([](httpp::server& s) {
        s.get("/who", [](const httpp::server_request& r, httpp::response& res) {
            auto creds = r.basic_auth();
            res.body = r.cookie("a") + "|" + r.cookie("missing") + "|" + std::to_string(r.cookies().size()) + "|" +
                       (creds ? creds->first + ":" + creds->second : std::string("none"));
        });
        s.get("/set", [](const httpp::server_request&, httpp::response& res) {
            res.set_cookie("a", "1");
            res.set_cookie(httpp::cookie("b", "2").set_max_age(60).set_same_site(httpp::same_site::lax));
            try { res.set_cookie("bad name", "1"); } catch (const std::invalid_argument&) { res.body = "refused"; }
        });
    });
    httpp::client c(srv.url());
    auto r = c.get("/who", {{"Cookie", "a=1; b=2; a=3"}, {"Authorization", "Basic dXNlcjpwYTpzcw=="}});
    ASSERT_STREQ("1||3|user:pa:ss", r.body.c_str());
    ASSERT_STREQ("||0|none", c.get("/who", {{"Authorization", "Bearer x"}}).body.c_str());
    ASSERT_STREQ("||0|none", c.get("/who", {{"Authorization", "Basic !!!"}}).body.c_str());
    auto set = c.get("/set");
    ASSERT_STREQ("refused", set.body.c_str());
    int seen = 0;
    for (const auto& [k, v] : set.headers) {
        if (k != "Set-Cookie") continue;
        seen++;
        if (v[0] == 'a') ASSERT_STREQ("a=1; Path=/; HttpOnly", v.c_str());
        else ASSERT_STREQ("b=2; Path=/; Max-Age=60; HttpOnly; SameSite=Lax", v.c_str());
    }
    ASSERT_EQ(2, seen);
}

UTEST(httpp_cookie, a_client_request_refuses_a_cookie_it_could_not_send) {
    httpp::request r("http://127.0.0.1:1/");
    ASSERT_TRUE(throws_invalid([&] { r.cookie("a b", "1"); }));
    ASSERT_TRUE(throws_invalid([&] { r.cookie("a", "x\r\nY: 1"); }));
    r.cookie("ok", "1");
}
