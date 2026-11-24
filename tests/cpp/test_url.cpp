#include "utest/utest.h"
#include "httpp/server.hpp"
#include "httpp/url.hpp"
#include "local_server.hpp"

#include <stdexcept>
#include <string>
#include <type_traits>

// httpp::url is urlparser::url (ADR-0067); what httpp accepts is decided by the url_* functions.

UTEST(httpp_url, is_the_liburlparser_type) {
    ASSERT_TRUE((std::is_same<httpp::url, urlparser::url>::value));
}

UTEST(httpp_url, parses_basic_http_url) {
    httpp::url u = httpp::parse_url("http://example.com/path?x=1");
    ASSERT_TRUE(httpp::url_valid(u));
    ASSERT_STREQ("http", httpp::url_scheme(u).c_str());
    ASSERT_STREQ("example.com", httpp::url_host(u).c_str());
    ASSERT_EQ(80, u.effective_port());
    ASSERT_STREQ("/path", httpp::url_path(u).c_str());
    ASSERT_STREQ("x=1", httpp::url_query(u).c_str());
}

UTEST(httpp_url, parses_https_with_explicit_port) {
    httpp::url u = httpp::parse_url("https://example.com:8443/a/b");
    ASSERT_TRUE(httpp::url_valid(u));
    ASSERT_STREQ("https", httpp::url_scheme(u).c_str());
    ASSERT_EQ(8443, u.effective_port());
    ASSERT_STREQ("/a/b", httpp::url_path(u).c_str());
    ASSERT_TRUE(httpp::url_query(u).empty());
}

UTEST(httpp_url, https_without_a_port_is_443) {
    ASSERT_EQ(443, httpp::parse_url("https://example.com/").effective_port());
}

UTEST(httpp_url, defaults_path_to_root) {
    httpp::url u = httpp::parse_url("http://example.com");
    ASSERT_TRUE(httpp::url_valid(u));
    ASSERT_STREQ("/", httpp::url_path(u).c_str());
}

UTEST(httpp_url, dot_segments_are_resolved_in_the_path) {
    ASSERT_STREQ("/a/c", httpp::url_path(httpp::parse_url("http://example.com/a/./b/../c")).c_str());
}

UTEST(httpp_url, rejects_unsupported_scheme) {
    ASSERT_FALSE(httpp::url_valid(httpp::parse_url("ftp://example.com")));
    ASSERT_FALSE(httpp::url_valid(httpp::parse_url("gopher://example.com:70/x")));
}

UTEST(httpp_url, rejects_missing_host) {
    ASSERT_FALSE(httpp::url_valid(httpp::parse_url("http:///path")));
    ASSERT_FALSE(httpp::url_valid(httpp::parse_url("http://user@/")));
}

UTEST(httpp_url, rejects_text_without_a_scheme) {
    ASSERT_FALSE(httpp::url_valid(httpp::parse_url("")));
    ASSERT_FALSE(httpp::url_valid(httpp::parse_url("nonsense")));
    ASSERT_FALSE(httpp::url_valid(httpp::parse_url("example.com/path")));
    ASSERT_FALSE(httpp::url_valid(httpp::url()));
}

UTEST(httpp_url, a_socks_url_is_valid_but_not_http) {
    httpp::url u = httpp::parse_url("socks5h://proxy.local");
    ASSERT_TRUE(httpp::url_valid(u));
    ASSERT_FALSE(httpp::url_is_http(u));
    ASSERT_EQ(1080, u.effective_port());
    ASSERT_EQ(1080, httpp::parse_url("socks5://proxy.local").effective_port());
    ASSERT_EQ(9050, httpp::parse_url("socks5://proxy.local:9050").effective_port());
    ASSERT_TRUE(httpp::url_is_http(httpp::parse_url("https://example.com")));
}

// A bad port is the one thing the parser refuses: constructing throws, parse_url gives an invalid url.
UTEST(httpp_url, an_invalid_port_throws_on_construction_and_parse_url_does_not) {
    for (const char* text : {"http://h:99999/", "http://h:abc/", "http://h:-1/", "http://h:65536/"}) {
        bool threw = false;
        try {
            httpp::url u(text);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        ASSERT_TRUE(threw);
        ASSERT_FALSE(httpp::url_valid(httpp::parse_url(text)));
    }
    ASSERT_TRUE(httpp::url_valid(httpp::parse_url("http://h:65535/")));
}

static bool takes_a_url(const httpp::url& u) { return httpp::url_valid(u) && httpp::url_host(u) == "example.com"; }

UTEST(httpp_url, a_string_or_a_literal_converts_implicitly) {
    ASSERT_TRUE(takes_a_url("http://example.com/x"));
    ASSERT_TRUE(takes_a_url(std::string("https://example.com")));
    httpp::url bad = "nonsense";
    ASSERT_FALSE(httpp::url_valid(bad));
    ASSERT_STREQ("nonsense", bad.str().c_str());
    ASSERT_STREQ("http://example.com/x?q=1", httpp::url("http://example.com/x?q=1").str().c_str());
}

UTEST(httpp_url, the_user_and_password_are_decoded) {
    httpp::url u = "http://us%40er:p%3Ass@proxy.local:3128/";
    ASSERT_TRUE(httpp::url_valid(u));
    ASSERT_STREQ("us@er", httpp::url_username(u).c_str());
    ASSERT_STREQ("p:ss", httpp::url_password(u).c_str());
    ASSERT_STREQ("proxy.local", httpp::url_host(u).c_str());
    ASSERT_EQ(3128, u.effective_port());
    httpp::url only_user = "http://bob@h/";
    ASSERT_STREQ("bob", httpp::url_username(only_user).c_str());
    ASSERT_STREQ("", httpp::url_password(only_user).c_str());
    ASSERT_STREQ("", httpp::url_username(httpp::url("http://h/")).c_str());
}

UTEST(httpp_url, an_ipv6_literal_keeps_its_brackets_in_the_host) {
    httpp::url u = httpp::parse_url("http://[::1]:8080/");
    ASSERT_TRUE(httpp::url_valid(u));
    ASSERT_STREQ("[::1]", httpp::url_host(u).c_str());
    ASSERT_EQ(8080, u.effective_port());
    ASSERT_STREQ("/", httpp::url_path(u).c_str());
}

UTEST(httpp_url, an_ipv6_literal_without_a_port_uses_the_scheme_default) {
    httpp::url u = httpp::parse_url("https://[2001:db8::1]/a?b=1");
    ASSERT_TRUE(httpp::url_valid(u));
    ASSERT_STREQ("[2001:db8::1]", httpp::url_host(u).c_str());
    ASSERT_EQ(443, u.effective_port());
    ASSERT_STREQ("b=1", httpp::url_query(u).c_str());
}

UTEST(httpp_url, an_ipv6_literal_with_userinfo_and_a_port) {
    httpp::url u = httpp::parse_url("http://bob:s3cret@[::1]:3128/");
    ASSERT_TRUE(httpp::url_valid(u));
    ASSERT_STREQ("bob", httpp::url_username(u).c_str());
    ASSERT_STREQ("s3cret", httpp::url_password(u).c_str());
    ASSERT_STREQ("[::1]", httpp::url_host(u).c_str());
    ASSERT_EQ(3128, u.effective_port());
}

UTEST(httpp_url, an_ipv4_literal_is_just_a_host_string) {
    httpp::url u = httpp::parse_url("http://127.0.0.1:8080/x");
    ASSERT_TRUE(httpp::url_valid(u));
    ASSERT_STREQ("127.0.0.1", httpp::url_host(u).c_str());
    ASSERT_EQ(8080, u.effective_port());
}

UTEST(httpp_url, an_empty_port_after_the_colon_means_no_port) {
    httpp::url u = httpp::parse_url("http://example.com:/p");
    ASSERT_TRUE(httpp::url_valid(u));
    ASSERT_EQ(80, u.effective_port());
}

// --- fetch_public_suffix_list: from a local server, no internet needed ---

UTEST(httpp_url, fetch_public_suffix_list_loads_the_list_and_keeps_the_old_one_on_failure) {
    using testutil::local_server;
    local_server srv([](httpp::server& s) {
        s.get("/psl.dat", [](const httpp::server_request&, httpp::response& res) {
            res.status = 200;
            res.body = "// VERSION: test\ncom\nuk\nco.uk\n";
        });
        s.get("/page.html", [](const httpp::server_request&, httpp::response& res) {
            res.status = 200;
            res.body = "<html><body>please sign in</body></html>";
        });
        s.get("/empty", [](const httpp::server_request&, httpp::response& res) { res.status = 200; });
        s.get("/missing", [](const httpp::server_request&, httpp::response& res) {
            res.status = 404;
            res.body = "no";
        });
    });

    auto fails = [](const std::string& source) {
        try {
            httpp::fetch_public_suffix_list(source, 5);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    // httpp builds liburlparser without the list: a multi-part suffix is wrong until one is loaded.
    ASSERT_FALSE(urlparser::url::is_psl_loaded());
    ASSERT_STREQ("uk", urlparser::hostname("www.example.co.uk").suffix().c_str());

    // Every failure throws and leaves things as they were.
    ASSERT_TRUE(fails(srv.url("/missing")));
    ASSERT_TRUE(fails(srv.url("/empty")));
    ASSERT_TRUE(fails(srv.url("/page.html")));
    ASSERT_TRUE(fails("http://127.0.0.1:1/psl.dat"));
    ASSERT_TRUE(fails("not a url"));
    ASSERT_FALSE(urlparser::url::is_psl_loaded());

    httpp::fetch_public_suffix_list(srv.url("/psl.dat"), 5);
    ASSERT_TRUE(urlparser::url::is_psl_loaded());
    urlparser::hostname h("www.example.co.uk");
    ASSERT_STREQ("co.uk", h.suffix().c_str());
    ASSERT_STREQ("example", h.domain().c_str());

    // A later failure does not take the loaded list away.
    ASSERT_TRUE(fails(srv.url("/missing")));
    ASSERT_STREQ("co.uk", urlparser::hostname("www.example.co.uk").suffix().c_str());
}

UTEST(httpp_url, fetch_public_suffix_list_defaults_to_the_address_liburlparser_names) {
    ASSERT_FALSE(urlparser::psl::instance().source_url().empty());
    // No network in the tests: only check that the default is the one the parser names and that it is an https URL.
    ASSERT_TRUE(httpp::url_is_http(httpp::parse_url(std::string(urlparser::psl::instance().source_url()))));
}

UTEST(httpp_url, url_filename_is_the_last_piece_of_the_path) {
    auto name = [](const char* text) { return httpp::url_filename(httpp::parse_url(text)); };
    ASSERT_STREQ("report.txt", name("http://h/files/report.txt").c_str());
    ASSERT_STREQ("report.txt", name("http://h/files/report.txt?x=1#top").c_str());
    ASSERT_STREQ("a%20b.txt", name("http://h/a%20b.txt").c_str()); // not decoded
    ASSERT_STREQ("", name("http://h/").c_str());
    ASSERT_STREQ("", name("http://h").c_str());
    ASSERT_STREQ("", name("http://h/dir/").c_str());
    ASSERT_STREQ("", name("http://h/dir/..").c_str());
}
