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
