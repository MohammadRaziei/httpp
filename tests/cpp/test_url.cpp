#include "utest/utest.h"
#include "httpp/url.hpp"

UTEST(httpp_url, parses_basic_http_url) {
    httpp::url u = httpp::url::parse("http://example.com/path?x=1");
    ASSERT_TRUE(u.valid());
    ASSERT_STREQ("http", u.scheme().c_str());
    ASSERT_STREQ("example.com", u.host().c_str());
    ASSERT_EQ(80, u.port());
    ASSERT_STREQ("/path", u.path().c_str());
    ASSERT_STREQ("x=1", u.query().c_str());
}

UTEST(httpp_url, parses_https_with_explicit_port) {
    httpp::url u = httpp::url::parse("https://example.com:8443/a/b");
    ASSERT_TRUE(u.valid());
    ASSERT_STREQ("https", u.scheme().c_str());
    ASSERT_EQ(8443, u.port());
    ASSERT_STREQ("/a/b", u.path().c_str());
    ASSERT_TRUE(u.query().empty());
}

UTEST(httpp_url, defaults_path_to_root) {
    httpp::url u = httpp::url::parse("http://example.com");
    ASSERT_TRUE(u.valid());
    ASSERT_STREQ("/", u.path().c_str());
}

UTEST(httpp_url, rejects_unsupported_scheme) {
    httpp::url u = httpp::url::parse("ftp://example.com");
    ASSERT_FALSE(u.valid());
}

UTEST(httpp_url, rejects_missing_host) {
    httpp::url u = httpp::url::parse("http:///path");
    ASSERT_FALSE(u.valid());
}

static bool takes_a_url(const httpp::url& u) { return u.valid() && u.host() == "example.com"; }

UTEST(httpp_url, a_string_or_a_literal_converts_implicitly_and_keeps_its_text) {
    ASSERT_TRUE(takes_a_url("http://example.com/x"));
    ASSERT_TRUE(takes_a_url(std::string("https://example.com")));
    httpp::url bad = "nonsense";
    ASSERT_FALSE(bad.valid());
    ASSERT_STREQ("nonsense", bad.str().c_str());
    ASSERT_STREQ("http://example.com/x?q=1", httpp::url("http://example.com/x?q=1").str().c_str());
    ASSERT_FALSE(httpp::url().valid());
}

UTEST(httpp_url, the_user_and_password_are_decoded) {
    httpp::url u = "http://us%40er:p%3Ass@proxy.local:3128/";
    ASSERT_TRUE(u.valid());
    ASSERT_STREQ("us@er", u.username().c_str());
    ASSERT_STREQ("p:ss", u.password().c_str());
    ASSERT_STREQ("proxy.local", u.host().c_str());
    ASSERT_EQ(3128, u.port());
    httpp::url only_user = "http://bob@h/";
    ASSERT_STREQ("bob", only_user.username().c_str());
    ASSERT_STREQ("", only_user.password().c_str());
    ASSERT_STREQ("", httpp::url("http://h/").username().c_str());
}
