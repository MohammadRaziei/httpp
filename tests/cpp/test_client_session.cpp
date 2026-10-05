// The client's richer API: explained failures, binary bodies, request options, sessions, async.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "httpp/url.hpp"
#include "local_server.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using testutil::local_server;

std::string header_of(const httpp::request& req, const std::string& name) {
    for (const auto& [k, v] : req.headers) {
        if (k.size() == name.size() &&
            std::equal(k.begin(), k.end(), name.begin(), [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }))
            return v;
    }
    return "";
}

// Echoes what the server saw: "METHOD target?query|content-type|authorization|cookie|body".
void echo_route(httpp::server& s) {
    auto handler = [](const httpp::request& req, httpp::response& res) {
        std::string query;
        for (const auto& [k, v] : req.query) query += (query.empty() ? "" : "&") + k + "=" + v;
        res.body = req.method + " " + req.path + "?" + query + "|" + header_of(req, "Content-Type") + "|" +
                   header_of(req, "Authorization") + "|" + header_of(req, "Cookie") + "|" + req.body + "|" +
                   header_of(req, "X-Default");
    };
    for (const char* m : {"GET", "POST", "PUT", "PATCH", "DELETE"}) s.route(m, "/echo", handler);
    s.route("GET", "/api/echo", handler);
}

} // namespace

UTEST(httpp_client_errors, a_refused_connection_says_why) {
    auto res = httpp::client::request("http://127.0.0.1:1/").timeout(3).run();
    ASSERT_EQ(0, res.status);
    ASSERT_TRUE(res.failed());
    ASSERT_STREQ("connection", res.error_name());
    ASSERT_FALSE(res.error_message.empty());
}

UTEST(httpp_client_errors, an_invalid_url_and_an_unsupported_method_say_why) {
    auto bad_url = httpp::client::request("not a url").run();
    ASSERT_STREQ("invalid_url", bad_url.error_name());

    local_server srv([](httpp::server& s) { echo_route(s); });
    auto bad_method = httpp::client::request(srv.url("/echo")).method("BREW").run();
    ASSERT_STREQ("unsupported_method", bad_method.error_name());
}

UTEST(httpp_client_errors, a_404_is_a_response_not_an_error) {
    local_server srv([](httpp::server&) {});
    auto res = httpp::client::fetch(srv.url("/nothing-here"));
    ASSERT_EQ(404, res.status);
    ASSERT_FALSE(res.failed());
    ASSERT_FALSE(res.ok());
}

UTEST(httpp_client_errors, a_slow_server_times_out) {
    local_server srv([](httpp::server& s) {
        s.get("/slow", [](const httpp::request&, httpp::response& res) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            res.body = "late";
        });
    });
    auto t0 = std::chrono::steady_clock::now();
    auto res = httpp::client::request(srv.url("/slow")).timeout(1).run();
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_STREQ("timeout", res.error_name());
    ASSERT_LT(seconds, 2.0);
}

UTEST(httpp_client_body, binary_bodies_survive_untouched) {
    std::string bytes;
    for (int i = 0; i < 1024; i++) bytes.push_back(static_cast<char>(i % 256));
    local_server srv([&](httpp::server& s) {
        s.get("/bin", [&](const httpp::request&, httpp::response& res) {
            res.body = bytes;
            res.headers.emplace_back("Content-Type", "application/octet-stream");
        });
        s.post("/len", [](const httpp::request& req, httpp::response& res) { res.body = std::to_string(req.body.size()); });
    });
    auto res = httpp::client::fetch(srv.url("/bin"));
    ASSERT_EQ(bytes.size(), res.body.size());
    ASSERT_TRUE(res.body == bytes);
    // and a binary request body, with NULs in it
    auto up = httpp::client::request(srv.url("/len")).data(std::string("a\0b\0c", 5)).run();
    ASSERT_STREQ("5", up.body.c_str());
}

UTEST(httpp_client_request, params_are_percent_encoded_and_json_form_auth_cookies_are_sent) {
    local_server srv([](httpp::server& s) { echo_route(s); });

    auto q = httpp::client::request(srv.url("/echo?keep=1")).param("name", "a b&c").param("x", "\xE2\x82\xAC").run();
    ASSERT_TRUE(q.body.find("keep=1") != std::string::npos);
    ASSERT_TRUE(q.body.find("name=a b&c") != std::string::npos); // the server saw it decoded
    ASSERT_TRUE(q.body.find("x=\xE2\x82\xAC") != std::string::npos);

    auto j = httpp::client::request(srv.url("/echo")).json("{\"a\":1}").run();
    ASSERT_TRUE(j.body.find("POST /echo?|application/json|") == 0);
    ASSERT_TRUE(j.body.find("|{\"a\":1}|") != std::string::npos);

    auto f = httpp::client::request(srv.url("/echo")).form({{"k", "v w"}, {"n", "1"}}).run();
    ASSERT_TRUE(f.body.find("application/x-www-form-urlencoded") != std::string::npos);

    auto basic = httpp::client::request(srv.url("/echo")).basic_auth("user", "pass").run();
    ASSERT_TRUE(basic.body.find("|Basic dXNlcjpwYXNz|") != std::string::npos);

    auto bearer = httpp::client::request(srv.url("/echo")).bearer("tok123").run();
    ASSERT_TRUE(bearer.body.find("|Bearer tok123|") != std::string::npos);

    auto cookies = httpp::client::request(srv.url("/echo")).cookie("a", "1").cookie("b", "2").run();
    ASSERT_TRUE(cookies.body.find("|a=1; b=2|") != std::string::npos);
}

UTEST(httpp_client_request, redirects_are_followed_only_when_asked_for) {
    local_server srv([](httpp::server& s) {
        s.get("/redir", [](const httpp::request&, httpp::response& res) {
            res.status = 302;
            res.headers.emplace_back("Location", "/target");
        });
        s.get("/target", [](const httpp::request&, httpp::response& res) { res.body = "arrived"; });
    });
    auto raw = httpp::client::request(srv.url("/redir")).run();
    ASSERT_EQ(302, raw.status);
    ASSERT_STREQ("/target", raw.header("Location").c_str());
    auto followed = httpp::client::request(srv.url("/redir")).follow_redirects().run();
    ASSERT_EQ(200, followed.status);
    ASSERT_STREQ("arrived", followed.body.c_str());
}

UTEST(httpp_client_session, base_url_defaults_methods_and_connection_reuse) {
    local_server srv([](httpp::server& s) { echo_route(s); });
    httpp::client c(srv.url("/api")); // a base path is prepended to every request path
    c.set_header("X-Default", "yes");
    auto r = c.get("/echo");
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(r.body.find("GET /api/echo?|") == 0);
    ASSERT_TRUE(r.body.find("|yes") != std::string::npos);

    httpp::client d(srv.url());
    ASSERT_TRUE(d.post("/echo", "hello").body.find("POST /echo") == 0);
    ASSERT_TRUE(d.put("/echo", "x").body.find("PUT /echo") == 0);
    ASSERT_TRUE(d.patch("/echo", "x").body.find("PATCH /echo") == 0);
    ASSERT_TRUE(d.del("/echo").body.find("DELETE /echo") == 0);
    ASSERT_EQ(200, d.head("/echo").status);
    for (int i = 0; i < 50; i++) ASSERT_EQ(200, d.get("/echo").status); // a kept-alive session keeps working
}

UTEST(httpp_client_session, a_malformed_base_url_throws) {
    bool threw = false;
    try { httpp::client c("ftp://nope"); } catch (const std::invalid_argument&) { threw = true; }
    ASSERT_TRUE(threw);
}

UTEST(httpp_client_async, run_async_returns_a_future_and_runs_requests_in_parallel) {
    local_server srv([](httpp::server& s) {
        s.get("/wait", [](const httpp::request&, httpp::response& res) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            res.body = "ok";
        });
    });
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::future<httpp::response>> futures;
    for (int i = 0; i < 8; i++) futures.push_back(httpp::client::request(srv.url("/wait")).run_async());
    int ok = 0;
    for (auto& f : futures) ok += f.get().status == 200;
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_EQ(8, ok);
    ASSERT_LT(seconds, 1.5); // eight 0.3 s requests overlapped instead of running one after another
}

UTEST(httpp_client_async, async_client_pools_connections_and_overlaps_requests) {
    std::atomic<int> concurrent{0}, peak{0};
    local_server srv([&](httpp::server& s) {
        s.get("/wait", [&](const httpp::request&, httpp::response& res) {
            int now = ++concurrent;
            int p = peak.load();
            while (now > p && !peak.compare_exchange_weak(p, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            --concurrent;
            res.body = "ok";
        });
    });
    httpp::async_client ac(srv.url(), 4); // at most 4 connections at a time
    std::vector<std::future<httpp::response>> futures;
    for (int i = 0; i < 12; i++) futures.push_back(ac.get("/wait"));
    int ok = 0;
    for (auto& f : futures) ok += f.get().status == 200;
    ASSERT_EQ(12, ok);
    ASSERT_LE(peak.load(), 4); // the pool bound is honoured
    ASSERT_GE(peak.load(), 2); // and requests really did overlap
}

UTEST(httpp_url, encode_decode_and_build_query) {
    ASSERT_STREQ("a%20b%26c", httpp::url::encode_component("a b&c").c_str());
    ASSERT_STREQ("-._~", httpp::url::encode_component("-._~").c_str());
    ASSERT_STREQ("a b&c", httpp::url::decode_component("a%20b%26c").c_str());
    ASSERT_STREQ("%zz", httpp::url::decode_component("%zz").c_str());
    ASSERT_STREQ("a=1&b=x%20y", httpp::url::build_query({{"a", "1"}, {"b", "x y"}}).c_str());
}
