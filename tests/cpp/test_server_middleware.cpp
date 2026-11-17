// Middleware: server::use() chaining, CORS and trusted host (ADR-0037).
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/middleware.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <asio.hpp>

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using testutil::local_server;
using headers_t = httpp::client::headers_t;

namespace {

// One raw HTTP/1.1 exchange with an arbitrary Host header; returns the whole answer (head and body).
std::string raw_get(int port, const std::string& host, const std::string& path = "/x") {
    asio::io_context io;
    asio::ip::tcp::socket s(io);
    s.connect({asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)});
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    asio::write(s, asio::buffer(req));
    std::string out;
    std::error_code ec;
    char buf[4096];
    for (;;) {
        std::size_t n = s.read_some(asio::buffer(buf), ec);
        if (n) out.append(buf, n);
        if (ec) break;
    }
    return out;
}

// True when `fn` throws std::invalid_argument.
template <class F> bool rejects(F fn) {
    try { fn(); } catch (const std::invalid_argument&) { return true; } catch (...) {}
    return false;
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

void ok_route(httpp::server& s) {
    s.get("/x", [](const httpp::server_request&, httpp::response& res) { res.body = "hello"; });
}

} // namespace

// ---------------------------------------------------------------- use()

UTEST(httpp_middleware_use, before_hooks_run_in_order_and_the_first_to_answer_wins) {
    local_server srv([](httpp::server& s) {
        ok_route(s);
        s.use([](const httpp::server_request& r, httpp::response& res) {
            if (r.query_param("a") == "1") { res.status = 418; res.body = "first"; return true; }
            return false;
        });
        s.use([](const httpp::server_request&, httpp::response& res) { res.status = 419; res.body = "second"; return true; });
    });
    auto a = httpp::client::fetch(srv.url("/x?a=1"));
    ASSERT_EQ(418, a.status);
    ASSERT_STREQ("first", a.body.c_str());
    auto b = httpp::client::fetch(srv.url("/x"));
    ASSERT_EQ(419, b.status);
    ASSERT_STREQ("second", b.body.c_str());
}

UTEST(httpp_middleware_use, after_hooks_all_run_and_the_applications_own_hooks_survive) {
    local_server srv([](httpp::server& s) {
        ok_route(s);
        s.use({}, [](const httpp::server_request&, httpp::response& res) { res.headers.emplace_back("X-One", "1"); });
        s.after([](const httpp::server_request&, httpp::response& res) { res.headers.emplace_back("X-App", "1"); });
        s.use({}, [](const httpp::server_request&, httpp::response& res) { res.headers.emplace_back("X-Two", "1"); });
        s.before([](const httpp::server_request& r, httpp::response& res) {
            if (r.query_param("stop") == "1") { res.status = 403; return true; }
            return false;
        });
    });
    auto r = httpp::client::fetch(srv.url("/x"));
    ASSERT_STREQ("1", r.header("X-One").c_str());
    ASSERT_STREQ("1", r.header("X-Two").c_str());
    ASSERT_STREQ("1", r.header("X-App").c_str());
    auto stopped = httpp::client::fetch(srv.url("/x?stop=1")); // the app's before hook still works, and the after hooks still run
    ASSERT_EQ(403, stopped.status);
    ASSERT_STREQ("1", stopped.header("X-One").c_str());
}

UTEST(httpp_middleware_use, a_server_without_hooks_is_unchanged) {
    local_server srv(ok_route);
    ASSERT_EQ(200, httpp::client::fetch(srv.url("/x")).status);
}

// ---------------------------------------------------------------- CORS

namespace {
void cors_site(httpp::server& s) {
    ok_route(s);
    httpp::cors().set_allow_origins({"https://a.example", "https://b.example"}).set_allow_methods({"GET", "POST"}).set_allow_headers({"X-Token"}).set_expose_headers({"X-Total"}).install(s);
}
} // namespace

UTEST(httpp_cors, an_allowed_preflight_is_answered_with_the_policy) {
    local_server srv(cors_site);
    httpp::client c(srv.url());
    auto r = c.options("/nowhere", {{"Origin", "https://a.example"}, {"Access-Control-Request-Method", "POST"},
                                    {"Access-Control-Request-Headers", "x-token, content-type"}});
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("https://a.example", r.header("Access-Control-Allow-Origin").c_str());
    ASSERT_TRUE(contains(r.header("Access-Control-Allow-Methods"), "POST"));
    ASSERT_TRUE(contains(r.header("Access-Control-Allow-Headers"), "X-Token"));
    ASSERT_STREQ("600", r.header("Access-Control-Max-Age").c_str());
    ASSERT_TRUE(contains(r.header("Vary"), "Origin"));
}

UTEST(httpp_cors, a_preflight_with_a_bad_origin_method_or_header_is_a_400_with_the_reason) {
    local_server srv(cors_site);
    httpp::client c(srv.url());
    auto origin = c.options("/x", {{"Origin", "https://evil.example"}, {"Access-Control-Request-Method", "GET"}});
    ASSERT_EQ(400, origin.status);
    ASSERT_TRUE(contains(origin.body, "Disallowed CORS origin"));
    auto method = c.options("/x", {{"Origin", "https://a.example"}, {"Access-Control-Request-Method", "DELETE"}});
    ASSERT_EQ(400, method.status);
    ASSERT_TRUE(contains(method.body, "Disallowed CORS method"));
    auto header = c.options("/x", {{"Origin", "https://a.example"}, {"Access-Control-Request-Method", "GET"},
                                   {"Access-Control-Request-Headers", "x-secret"}});
    ASSERT_EQ(400, header.status);
    ASSERT_TRUE(contains(header.body, "Disallowed CORS headers"));
}

UTEST(httpp_cors, an_options_request_that_is_not_a_preflight_goes_to_the_router) {
    local_server srv([](httpp::server& s) {
        s.route("OPTIONS", "/x", [](const httpp::server_request&, httpp::response& res) { res.status = 204; res.headers.emplace_back("Allow", "GET"); });
        httpp::cors().set_allow_origins({"https://a.example"}).install(s);
    });
    httpp::client c(srv.url());
    auto r = c.options("/x", {{"Origin", "https://a.example"}}); // no Access-Control-Request-Method
    ASSERT_EQ(204, r.status);
    ASSERT_STREQ("GET", r.header("Allow").c_str());
}

UTEST(httpp_cors, a_simple_request_gets_the_origin_header_only_when_allowed) {
    local_server srv(cors_site);
    httpp::client c(srv.url());
    auto ok = c.get("/x", {{"Origin", "https://b.example"}});
    ASSERT_EQ(200, ok.status);
    ASSERT_STREQ("https://b.example", ok.header("Access-Control-Allow-Origin").c_str());
    ASSERT_STREQ("X-Total", ok.header("Access-Control-Expose-Headers").c_str());
    ASSERT_TRUE(contains(ok.header("Vary"), "Origin"));

    auto bad = c.get("/x", {{"Origin", "https://evil.example"}});
    ASSERT_EQ(200, bad.status); // still served; the browser is the one that refuses it
    ASSERT_STREQ("", bad.header("Access-Control-Allow-Origin").c_str());

    auto none = c.get("/x"); // no Origin: not a cross-origin request
    ASSERT_STREQ("", none.header("Access-Control-Allow-Origin").c_str());
    ASSERT_STREQ("", none.header("Vary").c_str());
}

UTEST(httpp_cors, the_headers_are_also_on_error_answers) {
    local_server srv(cors_site);
    auto r = httpp::client(srv.url()).get("/missing", {{"Origin", "https://a.example"}});
    ASSERT_EQ(404, r.status);
    ASSERT_STREQ("https://a.example", r.header("Access-Control-Allow-Origin").c_str());
}

UTEST(httpp_cors, a_wildcard_sends_a_star_and_with_credentials_echoes_the_origin) {
    local_server star([](httpp::server& s) {
        ok_route(s);
        httpp::cors().set_allow_origins({"*"}).install(s);
    });
    auto a = httpp::client(star.url()).get("/x", {{"Origin", "https://any.example"}});
    ASSERT_STREQ("*", a.header("Access-Control-Allow-Origin").c_str());
    ASSERT_STREQ("", a.header("Access-Control-Allow-Credentials").c_str());

    local_server creds([](httpp::server& s) {
        ok_route(s);
        httpp::cors().set_allow_origins({"*"}).set_allow_credentials(true).install(s);
    });
    auto b = httpp::client(creds.url()).get("/x", {{"Origin", "https://any.example"}});
    ASSERT_STREQ("https://any.example", b.header("Access-Control-Allow-Origin").c_str());
    ASSERT_STREQ("true", b.header("Access-Control-Allow-Credentials").c_str());
    ASSERT_TRUE(contains(b.header("Vary"), "Origin"));
}

UTEST(httpp_cors, a_wildcard_header_list_echoes_what_the_preflight_asks_for_and_methods_star_allows_any) {
    local_server srv([](httpp::server& s) {
        ok_route(s);
        httpp::cors().set_allow_origins({"https://a.example"}).set_allow_methods({"*"}).set_allow_headers({"*"}).install(s);
    });
    auto r = httpp::client(srv.url()).options("/x", {{"Origin", "https://a.example"}, {"Access-Control-Request-Method", "PATCH"},
                                                     {"Access-Control-Request-Headers", "x-anything"}});
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(contains(r.header("Access-Control-Allow-Headers"), "x-anything"));
}

UTEST(httpp_cors, an_origin_regex_must_match_the_whole_origin) {
    local_server srv([](httpp::server& s) {
        ok_route(s);
        httpp::cors().set_allow_origin_regex(R"(https://.*\.example\.com)").install(s);
    });
    httpp::client c(srv.url());
    ASSERT_STREQ("https://app.example.com", c.get("/x", {{"Origin", "https://app.example.com"}}).header("Access-Control-Allow-Origin").c_str());
    ASSERT_STREQ("", c.get("/x", {{"Origin", "https://app.example.com.evil.io"}}).header("Access-Control-Allow-Origin").c_str());
}

UTEST(httpp_cors, bad_settings_throw_and_change_nothing) {
    httpp::cors c;
    c.set_allow_origin_regex("https://ok");
    ASSERT_TRUE(rejects([&] { c.set_allow_origin_regex("("); }));
    ASSERT_TRUE(rejects([&] { c.set_max_age(-1); }));
    // a refused value leaves the earlier one in place, and the chain still works
    httpp::server s;
    c.set_max_age(5).set_allow_credentials(true).install(s);
}

// ---------------------------------------------------------------- trusted host

UTEST(httpp_trusted_host, a_listed_host_passes_with_or_without_a_port_and_any_other_is_a_400) {
    local_server srv([](httpp::server& s) {
        ok_route(s);
        httpp::trusted_host({"example.com"}).install(s);
    });
    ASSERT_TRUE(contains(raw_get(srv.port, "example.com"), "200 OK"));
    ASSERT_TRUE(contains(raw_get(srv.port, "example.com:8080"), "200 OK"));
    ASSERT_TRUE(contains(raw_get(srv.port, "EXAMPLE.com"), "200 OK")); // host names are case-insensitive
    const std::string bad = raw_get(srv.port, "evil.com");
    ASSERT_TRUE(contains(bad, " 400 "));
    ASSERT_TRUE(contains(bad, "Invalid host header"));
}

UTEST(httpp_trusted_host, a_subdomain_wildcard_matches_subdomains_only_and_star_matches_everything) {
    local_server sub([](httpp::server& s) {
        ok_route(s);
        httpp::trusted_host({"*.example.com"}).set_www_redirect(false).install(s);
    });
    ASSERT_TRUE(contains(raw_get(sub.port, "api.example.com"), "200 OK"));
    ASSERT_TRUE(contains(raw_get(sub.port, "a.b.example.com"), "200 OK"));
    ASSERT_TRUE(contains(raw_get(sub.port, "example.com"), " 400 "));
    ASSERT_TRUE(contains(raw_get(sub.port, "notexample.com"), " 400 "));

    local_server any([](httpp::server& s) {
        ok_route(s);
        httpp::trusted_host({"*"}).set_www_redirect(false).install(s);
    });
    ASSERT_TRUE(contains(raw_get(any.port, "whatever.test"), "200 OK"));
}

UTEST(httpp_trusted_host, www_redirect_goes_to_the_allowed_spelling) {
    local_server srv([](httpp::server& s) {
        ok_route(s);
        httpp::trusted_host({"www.example.com"}).install(s);
    });
    const std::string r = raw_get(srv.port, "example.com", "/x?a=1");
    ASSERT_TRUE(contains(r, " 307 "));
    ASSERT_TRUE(contains(r, "Location: http://www.example.com/x?a=1"));
}

UTEST(httpp_trusted_host, bad_options_throw) {
    httpp::server s;
    ASSERT_TRUE(rejects([&] { httpp::trusted_host(std::vector<std::string>{}); }));
    ASSERT_TRUE(rejects([&] { httpp::trusted_host({"*example.com"}); }));
    ASSERT_TRUE(rejects([&] { httpp::trusted_host({"a.*.com"}); }));
}

// ---------------------------------------------------------------- forwarded headers (ADR-0069)

namespace {

void who_route(httpp::server& s) {
    s.get("/who", [](const httpp::server_request& r, httpp::response& res) {
        res.body = r.client_ip + "|" + r.scheme() + "|" + r.header("Host");
    });
}

// "client|scheme|host" as the handler saw them, for a request that carries `extra` raw header lines.
std::string who(int port, const std::string& extra) {
    asio::io_context io;
    asio::ip::tcp::socket s(io);
    s.connect({asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)});
    const std::string req = "GET /who HTTP/1.1\r\nHost: internal:8000\r\n" + extra + "Connection: close\r\n\r\n";
    asio::write(s, asio::buffer(req));
    std::string out;
    std::error_code ec;
    char buf[4096];
    for (;;) {
        const std::size_t n = s.read_some(asio::buffer(buf), ec);
        if (n) out.append(buf, n);
        if (ec) break;
    }
    const auto body = out.find("\r\n\r\n");
    return body == std::string::npos ? out : out.substr(body + 4);
}

} // namespace

UTEST(httpp_forwarded_headers, without_it_the_client_is_the_connection_and_the_headers_do_nothing) {
    local_server srv([](httpp::server& s) { who_route(s); });
    ASSERT_STREQ("127.0.0.1|http|internal:8000", who(srv.port, "X-Forwarded-For: 203.0.113.9\r\nX-Forwarded-Proto: https\r\n").c_str());
}

UTEST(httpp_forwarded_headers, a_trusted_connection_gives_the_client_the_scheme_and_with_a_switch_the_host) {
    local_server srv([](httpp::server& s) { who_route(s); httpp::forwarded_headers().install(s); }); // loopback is trusted by default
    ASSERT_STREQ("203.0.113.9|https|internal:8000",
                 who(srv.port, "X-Forwarded-For: 203.0.113.9\r\nX-Forwarded-Proto: https\r\nX-Forwarded-Host: shop.example\r\n").c_str());
    local_server with_host([](httpp::server& s) { who_route(s); httpp::forwarded_headers().set_trust_forwarded_host(true).install(s); });
    ASSERT_STREQ("203.0.113.9|http|shop.example",
                 who(with_host.port, "X-Forwarded-For: 203.0.113.9\r\nX-Forwarded-Host: a.example, shop.example\r\n").c_str());
}

UTEST(httpp_forwarded_headers, the_client_is_the_first_address_from_the_right_that_is_not_a_trusted_proxy) {
    local_server srv([](httpp::server& s) {
        who_route(s);
        httpp::forwarded_headers().set_trusted_proxies({"127.0.0.1", "10.0.0.0/8", "fd00::/8"}).install(s);
    });
    // a forged leftmost value does not win: 203.0.113.9 is what the proxy next to us saw
    ASSERT_STREQ("203.0.113.9|http|internal:8000", who(srv.port, "X-Forwarded-For: 1.1.1.1, 203.0.113.9, 10.1.2.3, fd00::5\r\n").c_str());
    ASSERT_STREQ("10.1.2.3|http|internal:8000", who(srv.port, "X-Forwarded-For: 10.1.2.3, 10.9.9.9\r\n").c_str()); // all trusted: the leftmost
    ASSERT_STREQ("127.0.0.1|http|internal:8000", who(srv.port, "X-Forwarded-For: not-an-address\r\n").c_str());       // ignored
}

UTEST(httpp_forwarded_headers, a_connection_that_is_not_trusted_is_left_alone) {
    local_server srv([](httpp::server& s) { who_route(s); httpp::forwarded_headers().set_trusted_proxies({"10.0.0.0/8"}).install(s); });
    ASSERT_STREQ("127.0.0.1|http|internal:8000", who(srv.port, "X-Forwarded-For: 203.0.113.9\r\nX-Forwarded-Proto: https\r\n").c_str());
    local_server none([](httpp::server& s) { who_route(s); httpp::forwarded_headers().set_trusted_proxies({}).install(s); });
    ASSERT_STREQ("127.0.0.1|http|internal:8000", who(none.port, "X-Forwarded-For: 203.0.113.9\r\n").c_str());
    local_server any([](httpp::server& s) { who_route(s); httpp::forwarded_headers().set_trusted_proxies({"*"}).install(s); });
    ASSERT_STREQ("203.0.113.9|http|internal:8000", who(any.port, "X-Forwarded-For: 203.0.113.9\r\n").c_str());
}

UTEST(httpp_forwarded_headers, bad_entries_are_refused_and_change_nothing) {
    httpp::forwarded_headers f;
    for (const char* bad : {"localhost", "10.0.0.0/33", "10.0.0.0/", "::1/129", "10.0.0.0/x", "", "1.2.3"})
        ASSERT_TRUE(rejects([&] { f.set_trusted_proxies({bad}); }));
    f.set_trusted_proxies({"192.168.0.0/16", "2001:db8::/32", "::1", "127.0.0.1/32"}).set_trust_forwarded_host(true);
    httpp::server s;
    f.install(s);
    f.install(s); // a second install replaces the first
}

// ---------------------------------------------------------------- basic_auth

namespace {
void auth_site(httpp::server& s) {
    ok_route(s);
    s.get("/admin/panel", [](const httpp::server_request&, httpp::response& res) { res.body = "panel"; });
    s.get("/administrator", [](const httpp::server_request&, httpp::response& res) { res.body = "open"; });
    httpp::basic_auth("Tools").set_users({"alice:s3cret", "bob:pa:ss", u8"علی:رمز"}).set_path_prefix("/admin/").install(s);
}
std::string base64_text(const std::string& in) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(in[i]);
        const unsigned b = i + 1 < in.size() ? static_cast<unsigned char>(in[i + 1]) : 0;
        const unsigned c = i + 2 < in.size() ? static_cast<unsigned char>(in[i + 2]) : 0;
        const unsigned t = (a << 16) | (b << 8) | c;
        out += tab[(t >> 18) & 63];
        out += tab[(t >> 12) & 63];
        out += i + 1 < in.size() ? tab[(t >> 6) & 63] : '=';
        out += i + 2 < in.size() ? tab[t & 63] : '=';
    }
    return out;
}
std::string basic(const std::string& user_pass) { return "Basic " + base64_text(user_pass); }
} // namespace

UTEST(httpp_basic_auth, a_protected_path_needs_a_known_user_and_the_right_password) {
    local_server srv(auth_site);
    httpp::client c(srv.url());
    auto none = c.get("/admin/panel");
    ASSERT_EQ(401, none.status);
    ASSERT_STREQ("Basic realm=\"Tools\", charset=\"UTF-8\"", none.header("WWW-Authenticate").c_str());
    ASSERT_EQ(401, c.get("/admin/panel", {{"Authorization", basic("alice:wrong")}}).status);
    ASSERT_EQ(401, c.get("/admin/panel", {{"Authorization", basic("carol:s3cret")}}).status);
    ASSERT_EQ(401, c.get("/admin/panel", {{"Authorization", basic("alice")}}).status);       // no ':' in the decoded text
    ASSERT_EQ(401, c.get("/admin/panel", {{"Authorization", "Basic !!!!"}}).status);          // not base64
    ASSERT_EQ(401, c.get("/admin/panel", {{"Authorization", "Bearer abc"}}).status);
    auto ok = c.get("/admin/panel", {{"Authorization", basic("alice:s3cret")}});
    ASSERT_EQ(200, ok.status);
    ASSERT_STREQ("panel", ok.body.c_str());
    ASSERT_EQ(200, c.get("/admin/panel", {{"Authorization", "basic " + basic("bob:pa:ss").substr(6)}}).status); // scheme in any case, ':' inside the password
    ASSERT_EQ(200, c.get("/admin/panel", {{"Authorization", basic(u8"علی:رمز")}}).status);
}

UTEST(httpp_basic_auth, only_paths_under_the_prefix_are_checked) {
    local_server srv(auth_site);
    httpp::client c(srv.url());
    ASSERT_EQ(200, c.get("/x").status);
    ASSERT_EQ(200, c.get("/administrator").status); // "/admin" is a prefix of the text but not of the path
}

UTEST(httpp_basic_auth, the_whole_site_is_checked_by_default) {
    local_server srv([](httpp::server& s) { ok_route(s); httpp::basic_auth().set_users({"u:p"}).install(s); });
    httpp::client c(srv.url());
    auto r = c.get("/x");
    ASSERT_EQ(401, r.status);
    ASSERT_STREQ("Basic realm=\"Restricted\", charset=\"UTF-8\"", r.header("WWW-Authenticate").c_str());
    ASSERT_EQ(200, c.get("/x", {{"Authorization", basic("u:p")}}).status);
}

UTEST(httpp_basic_auth, bad_settings_are_refused_and_change_nothing) {
    httpp::basic_auth a;
    ASSERT_TRUE(rejects([&] { a.set_realm("a\"b"); }));
    ASSERT_TRUE(rejects([&] { a.set_realm("a\\b"); }));
    ASSERT_TRUE(rejects([&] { a.set_realm("a\nb"); }));
    ASSERT_TRUE(rejects([&] { a.set_users({"nocolon"}); }));
    ASSERT_TRUE(rejects([&] { a.set_users({":nouser"}); }));
    ASSERT_TRUE(rejects([&] { a.set_path_prefix("admin"); }));
    ASSERT_TRUE(rejects([&] { a.set_path_prefix(""); }));
    httpp::server s;
    ASSERT_TRUE(rejects([&] { a.install(s); })); // no users
}

// ---------------------------------------------------------------- rate_limit

UTEST(httpp_rate_limit, a_burst_passes_then_429_with_retry_after_and_the_bucket_refills) {
    local_server srv([](httpp::server& s) { ok_route(s); httpp::rate_limit().set_rate(10).set_burst(2).install(s); });
    httpp::client c(srv.url());
    ASSERT_EQ(200, c.get("/x").status);
    ASSERT_EQ(200, c.get("/x").status);
    auto limited = c.get("/x");
    ASSERT_EQ(429, limited.status);
    ASSERT_STREQ("1", limited.header("Retry-After").c_str());
    ASSERT_STREQ("Too Many Requests", limited.body.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(250)); // 10 per second: two or three tokens are back
    ASSERT_EQ(200, c.get("/x").status);
}

UTEST(httpp_rate_limit, a_slow_rate_asks_for_a_longer_wait) {
    local_server srv([](httpp::server& s) { ok_route(s); httpp::rate_limit().set_rate(0.1).set_burst(1).install(s); });
    httpp::client c(srv.url());
    ASSERT_EQ(200, c.get("/x").status);
    auto limited = c.get("/x");
    ASSERT_EQ(429, limited.status);
    const int wait = std::stoi(limited.header("Retry-After"));
    ASSERT_TRUE(wait >= 9 && wait <= 10);
}

UTEST(httpp_rate_limit, clients_are_told_apart_by_the_forwarded_address) {
    local_server srv([](httpp::server& s) {
        ok_route(s);
        httpp::forwarded_headers().set_trusted_proxies({"*"}).install(s);
        httpp::rate_limit().set_rate(0.01).set_burst(1).install(s);
    });
    httpp::client c(srv.url());
    ASSERT_EQ(200, c.get("/x", {{"X-Forwarded-For", "203.0.113.1"}}).status);
    ASSERT_EQ(429, c.get("/x", {{"X-Forwarded-For", "203.0.113.1"}}).status);
    ASSERT_EQ(200, c.get("/x", {{"X-Forwarded-For", "203.0.113.2"}}).status);
}

UTEST(httpp_rate_limit, a_small_table_stays_small_and_keeps_serving) {
    local_server srv([](httpp::server& s) {
        ok_route(s);
        httpp::forwarded_headers().set_trusted_proxies({"*"}).install(s);
        httpp::rate_limit().set_rate(0.01).set_burst(1).set_max_clients(3).install(s);
    });
    httpp::client c(srv.url());
    for (int i = 1; i <= 20; i++) ASSERT_EQ(200, c.get("/x", {{"X-Forwarded-For", "198.51.100." + std::to_string(i)}}).status);
}

UTEST(httpp_rate_limit, bad_settings_are_refused) {
    httpp::rate_limit r;
    ASSERT_TRUE(rejects([&] { r.set_rate(0); }));
    ASSERT_TRUE(rejects([&] { r.set_rate(-1); }));
    ASSERT_TRUE(rejects([&] { r.set_rate(std::nan("")); }));
    ASSERT_TRUE(rejects([&] { r.set_burst(0); }));
    ASSERT_TRUE(rejects([&] { r.set_max_clients(0); }));
}

UTEST(httpp_basic_auth, a_verifier_decides_and_can_work_with_or_without_users) {
    local_server srv([](httpp::server& s) {
        ok_route(s);
        httpp::basic_auth().set_verifier([](const std::string& u, const std::string& p) { return u == "dbuser" && p == "dbpass"; }).install(s);
    });
    httpp::client c(srv.url());
    ASSERT_EQ(401, c.get("/x").status);
    ASSERT_EQ(401, c.get("/x", {{"Authorization", basic("dbuser:wrong")}}).status);
    ASSERT_EQ(200, c.get("/x", {{"Authorization", basic("dbuser:dbpass")}}).status);

    local_server both([](httpp::server& s) {
        ok_route(s);
        httpp::basic_auth().set_users({"fixed:one"}).set_verifier([](const std::string& u, const std::string&) { return u == "other"; }).install(s);
    });
    httpp::client c2(both.url());
    ASSERT_EQ(200, c2.get("/x", {{"Authorization", basic("fixed:one")}}).status);
    ASSERT_EQ(200, c2.get("/x", {{"Authorization", basic("other:anything")}}).status);
    ASSERT_EQ(401, c2.get("/x", {{"Authorization", basic("nobody:x")}}).status);
}

UTEST(httpp_basic_auth, a_throwing_verifier_never_lets_the_request_in) {
    local_server srv([](httpp::server& s) {
        ok_route(s);
        httpp::basic_auth().set_verifier([](const std::string&, const std::string&) -> bool { throw std::runtime_error("db down"); }).install(s);
    });
    httpp::client c(srv.url());
    ASSERT_EQ(500, c.get("/x", {{"Authorization", basic("u:p")}}).status);
    ASSERT_EQ(401, c.get("/x").status); // no credentials: the verifier is not even asked
}
