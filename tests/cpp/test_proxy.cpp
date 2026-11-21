#include "utest/utest.h"
#include "httpp/proxy.hpp"

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <cctype>
#include <string>

// httpp::proxy_from_environment and httpp::resolve_proxy: the proxy variables and no_proxy list as curl reads them (ADR-0067).

namespace {

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : ""); // an empty value removes the variable
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

// Starts every test with none of the proxy variables set, and puts them back (unset) at the end.
struct clean_environment {
    static void clear() {
        for (const char* name : {"http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "no_proxy", "NO_PROXY"})
            set_env(name, nullptr);
    }
    clean_environment() { clear(); }
    ~clean_environment() { clear(); }
};

// "scheme://host:port" of a proxy, or "(direct)": url::str() normalizes the text (a trailing "/"), so it is not compared.
std::string describe(const std::optional<httpp::url>& p) {
    if (!p) return "(direct)";
    return httpp::url_scheme(*p) + "://" + httpp::url_host(*p) + ":" + std::to_string(httpp::url_port(*p));
}

std::string proxy_for(const std::string& target) { return describe(httpp::proxy_from_environment(target)); }

} // namespace

UTEST(httpp_proxy, no_variables_means_a_direct_connection) {
    clean_environment env;
    ASSERT_STREQ("(direct)", proxy_for("http://example.com/").c_str());
    ASSERT_STREQ("(direct)", proxy_for("https://example.com/").c_str());
}

UTEST(httpp_proxy, http_and_https_targets_read_their_own_variable) {
    clean_environment env;
    set_env("http_proxy", "http://127.0.0.1:1111");
    set_env("https_proxy", "http://127.0.0.1:2222");
    ASSERT_STREQ("http://127.0.0.1:1111", proxy_for("http://example.com/").c_str());
    ASSERT_STREQ("http://127.0.0.1:2222", proxy_for("https://example.com/").c_str());
}

UTEST(httpp_proxy, uppercase_HTTP_PROXY_is_ignored_like_curl_but_HTTPS_PROXY_is_read) {
    clean_environment env;
    set_env("HTTP_PROXY", "http://127.0.0.1:1111"); // httpoxy: a CGI server turns the Proxy header into this variable
    set_env("HTTPS_PROXY", "http://127.0.0.1:2222");
    ASSERT_STREQ("(direct)", proxy_for("http://example.com/").c_str());
    ASSERT_STREQ("http://127.0.0.1:2222", proxy_for("https://example.com/").c_str());
}

UTEST(httpp_proxy, all_proxy_is_the_fallback_for_both_schemes) {
    clean_environment env;
    set_env("ALL_PROXY", "socks5h://127.0.0.1:9050");
    ASSERT_STREQ("socks5h://127.0.0.1:9050", proxy_for("http://example.com/").c_str());
    ASSERT_STREQ("socks5h://127.0.0.1:9050", proxy_for("https://example.com/").c_str());
    set_env("http_proxy", "http://127.0.0.1:1111"); // the scheme's own variable wins
    ASSERT_STREQ("http://127.0.0.1:1111", proxy_for("http://example.com/").c_str());
    ASSERT_STREQ("socks5h://127.0.0.1:9050", proxy_for("https://example.com/").c_str());
}

UTEST(httpp_proxy, an_empty_variable_counts_as_unset) {
    clean_environment env;
    set_env("http_proxy", "");
    set_env("all_proxy", "http://127.0.0.1:3333");
    ASSERT_STREQ("http://127.0.0.1:3333", proxy_for("http://example.com/").c_str());
}

UTEST(httpp_proxy, a_value_without_a_scheme_is_an_http_proxy_like_in_curl) {
    clean_environment env;
    set_env("http_proxy", "127.0.0.1:10808");
    ASSERT_STREQ("http://127.0.0.1:10808", proxy_for("http://example.com/").c_str());
    set_env("http_proxy", "user:pa%20ss@proxy.local:3128");
    std::optional<httpp::url> p = httpp::proxy_from_environment(httpp::url("http://example.com/"));
    ASSERT_TRUE(p.has_value());
    ASSERT_STREQ("proxy.local", httpp::url_host(*p).c_str());
    ASSERT_EQ(3128, httpp::url_port(*p));
    ASSERT_STREQ("user", httpp::url_username(*p).c_str());
    ASSERT_STREQ("pa ss", httpp::url_password(*p).c_str());
}

UTEST(httpp_proxy, an_unusable_proxy_value_throws_and_names_it) {
    clean_environment env;
    set_env("http_proxy", "ftp://127.0.0.1:21");
    bool threw = false;
    try {
        proxy_for("http://example.com/");
    } catch (const std::invalid_argument& e) {
        threw = std::string(e.what()).find("ftp://127.0.0.1:21") != std::string::npos;
    }
    ASSERT_TRUE(threw);
    set_env("http_proxy", "https://127.0.0.1:443"); // an https proxy is not offered (ADR-0067)
    threw = false;
    try {
        proxy_for("http://example.com/");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

UTEST(httpp_proxy, no_proxy_star_excludes_everything) {
    clean_environment env;
    set_env("http_proxy", "http://127.0.0.1:1111");
    set_env("no_proxy", "*");
    ASSERT_STREQ("(direct)", proxy_for("http://example.com/").c_str());
}

UTEST(httpp_proxy, no_proxy_names_match_the_host_and_its_subdomains_not_other_hosts) {
    clean_environment env;
    set_env("http_proxy", "http://127.0.0.1:1111");
    set_env("no_proxy", "example.com,.internal.test, Local.Host");
    ASSERT_STREQ("(direct)", proxy_for("http://example.com/").c_str());
    ASSERT_STREQ("(direct)", proxy_for("http://www.example.com/x").c_str());
    ASSERT_STREQ("(direct)", proxy_for("http://EXAMPLE.com/").c_str());
    ASSERT_STREQ("(direct)", proxy_for("http://a.b.internal.test/").c_str());
    ASSERT_STREQ("(direct)", proxy_for("http://internal.test/").c_str()); // the leading dot is ignored, like curl
    ASSERT_STREQ("(direct)", proxy_for("http://local.host/").c_str());
    ASSERT_STREQ("http://127.0.0.1:1111", proxy_for("http://notexample.com/").c_str()); // a suffix must start at a label
    ASSERT_STREQ("http://127.0.0.1:1111", proxy_for("http://example.com.evil.test/").c_str());
    ASSERT_STREQ("http://127.0.0.1:1111", proxy_for("http://parrot.live/").c_str());
}

UTEST(httpp_proxy, no_proxy_takes_addresses_and_ipv4_cidr_ranges) {
    clean_environment env;
    set_env("http_proxy", "http://127.0.0.1:1111");
    set_env("NO_PROXY", "localhost,127.0.0.0/8,::1,10.1.2.3 192.168.0.0/16");
    ASSERT_STREQ("(direct)", proxy_for("http://localhost:8080/").c_str());
    ASSERT_STREQ("(direct)", proxy_for("http://127.0.0.1:8080/").c_str());
    ASSERT_STREQ("(direct)", proxy_for("http://127.9.9.9/").c_str());
    ASSERT_STREQ("(direct)", proxy_for("http://10.1.2.3/").c_str());
    ASSERT_STREQ("(direct)", proxy_for("http://192.168.77.5/").c_str());
    ASSERT_STREQ("http://127.0.0.1:1111", proxy_for("http://10.1.2.4/").c_str());
    ASSERT_STREQ("http://127.0.0.1:1111", proxy_for("http://192.169.0.1/").c_str());
    ASSERT_STREQ("http://127.0.0.1:1111", proxy_for("http://parrot.live/").c_str()); // the case that mattered: his no_proxy has loopback only
}

UTEST(httpp_proxy, lowercase_no_proxy_is_read_before_uppercase) {
    clean_environment env;
    set_env("http_proxy", "http://127.0.0.1:1111");
    set_env("no_proxy", "a.test");
    set_env("NO_PROXY", "b.test");
    ASSERT_STREQ("(direct)", proxy_for("http://a.test/").c_str());
    ASSERT_STREQ("http://127.0.0.1:1111", proxy_for("http://b.test/").c_str());
}

UTEST(httpp_proxy, resolve_proxy_with_an_explicit_proxy_replaces_the_variables) {
    clean_environment env;
    set_env("http_proxy", "http://127.0.0.1:1111");
    httpp::url target("http://example.com/");
    auto given = httpp::resolve_proxy(target, std::string("socks5://127.0.0.1:1080"), std::nullopt);
    ASSERT_STREQ("socks5://127.0.0.1:1080", describe(given).c_str());
    auto bare = httpp::resolve_proxy(target, std::string("proxy.local:3128"), std::nullopt);
    ASSERT_STREQ("http://proxy.local:3128", describe(bare).c_str());
    ASSERT_FALSE(httpp::resolve_proxy(target, std::string(""), std::nullopt).has_value()); // curl -x "": no proxy at all
}

UTEST(httpp_proxy, resolve_proxy_with_an_explicit_no_proxy_replaces_the_variable_and_still_applies_to_an_explicit_proxy) {
    clean_environment env;
    set_env("http_proxy", "http://127.0.0.1:1111");
    set_env("NO_PROXY", "example.com");
    httpp::url target("http://example.com/");
    ASSERT_FALSE(httpp::resolve_proxy(target, std::nullopt, std::nullopt).has_value());
    ASSERT_TRUE(httpp::resolve_proxy(target, std::nullopt, std::string("other.test")).has_value());
    ASSERT_TRUE(httpp::resolve_proxy(target, std::nullopt, std::string("")).has_value()); // curl --noproxy "": nothing is excluded
    ASSERT_FALSE(httpp::resolve_proxy(target, std::string("http://127.0.0.1:2222"), std::string("example.com")).has_value());
    ASSERT_TRUE(httpp::resolve_proxy(target, std::string("http://127.0.0.1:2222"), std::string("other.test")).has_value());
}

// direct:// is another way to say "no proxy at all" when it is given explicitly, as "" is (ADR-0106). A scheme and nothing else:
// the bare words "direct" and "off" have no scheme and are hosts, and an environment variable never turns the proxy off this way.

UTEST(httpp_proxy, an_explicit_direct_scheme_means_no_proxy_at_all_like_an_empty_string) {
    clean_environment env;
    set_env("http_proxy", "http://127.0.0.1:1111");
    set_env("all_proxy", "http://127.0.0.1:3333");
    httpp::url target("http://example.com/");
    // the parser reads "direct:", "direct:/" and "direct://" the same, in any case, and trims the text
    for (const char* text : {"direct://", "DIRECT://", "Direct://", "  direct://  ", "direct:///", "direct:/", "direct:"}) {
        ASSERT_FALSE(httpp::resolve_proxy(target, std::string(text), std::nullopt).has_value());
    }
    ASSERT_FALSE(httpp::resolve_proxy(httpp::url("https://example.com/"), std::string("direct://"), std::nullopt).has_value());
}

UTEST(httpp_proxy, direct_with_anything_after_the_scheme_is_an_error_not_a_proxy) {
    clean_environment env;
    httpp::url target("http://example.com/");
    // a host, a port, a user, a path, a query or a fragment: the text is not "no proxy", and direct is no proxy scheme
    for (const char* text : {"direct://host", "direct://:3128", "direct://host:3128", "direct://u@", "direct://u:p@", "direct:///x",
                             "direct://?a=1", "direct://#f", "direct://[::1]"}) {
        bool threw = false;
        try {
            httpp::resolve_proxy(target, std::string(text), std::nullopt);
        } catch (const std::invalid_argument& e) {
            threw = true;
            ASSERT_NE(std::string::npos, std::string(e.what()).find("direct://")); // says what is wrong, not the parser's rewrite of the text
        }
        ASSERT_TRUE(threw);
    }
}

UTEST(httpp_proxy, the_bare_words_direct_and_off_are_hosts_like_any_other) {
    clean_environment env;
    httpp::url target("http://example.com/");
    // (the parser writes a host name in lower case, so "DIRECT" comes back as "direct")
    for (const char* word : {"direct", "off", "DIRECT"}) {
        auto p = httpp::resolve_proxy(target, std::string(word), std::nullopt);
        ASSERT_TRUE(p.has_value());
        std::string expected(word);
        for (auto& ch : expected) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        ASSERT_STREQ(expected.c_str(), httpp::url_host(*p).c_str());
    }
    auto with_scheme = httpp::resolve_proxy(target, std::string("http://direct"), std::nullopt);
    ASSERT_TRUE(with_scheme.has_value());
    ASSERT_STREQ("direct", httpp::url_host(*with_scheme).c_str());
    auto with_port = httpp::resolve_proxy(target, std::string("direct:3128"), std::nullopt);
    ASSERT_TRUE(with_port.has_value());
    ASSERT_STREQ("direct", httpp::url_host(*with_port).c_str());
    ASSERT_EQ(3128, static_cast<int>(httpp::url_port(*with_port)));
}

UTEST(httpp_proxy, direct_in_the_environment_is_an_error_it_only_works_when_it_is_given_explicitly) {
    clean_environment env;
    set_env("http_proxy", "direct://");
    bool threw = false;
    try {
        httpp::proxy_from_environment(httpp::url("http://example.com/"));
    } catch (const std::invalid_argument& e) {
        threw = true;
        ASSERT_NE(std::string::npos, std::string(e.what()).find("direct://"));
    }
    ASSERT_TRUE(threw);
}
