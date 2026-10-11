// request::run_async() and async_client on the non-blocking engine: request building, redirects,
// credentials, concurrency cap, errors.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "local_server.hpp"
#include "tls_test_server.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace std::chrono_literals;

namespace {

httpp::response get(std::future<httpp::response>& f, int seconds = 20) {
    if (f.wait_for(std::chrono::seconds(seconds)) != std::future_status::ready) {
        httpp::response r;
        r.error = httpp::error_kind::timeout;
        r.error_message = "test gave up waiting";
        return r;
    }
    return f.get();
}

// Answers with what it received, one field per line.
void add_echo(httpp::server& s) {
    auto echo = [](const httpp::request& q, httpp::response& r) {
        r.status = 200;
        r.body = "method=" + q.method + "\npath=" + q.path + "\nbody=" + q.body + "\nauth=" + q.header("Authorization") +
                 "\ncookie=" + q.header("Cookie") + "\nctype=" + q.header("Content-Type") + "\nx=" + q.header("X-Test") +
                 "\nua=" + q.header("User-Agent") + "\nq=" + q.query_param("a") + "," + q.query_param("b") + "\n";
    };
    for (const char* m : {"GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"}) s.route(m, "/echo", echo);
}

bool has(const httpp::response& r, const std::string& line) { return r.body.find(line + "\n") != std::string::npos; }

} // namespace

// ------------------------------------------------------------------ request building

UTEST(client_async, builds_the_request_from_the_builder) {
    testutil::local_server srv(add_echo);
    auto f = httpp::client::request(srv.url("/echo?a=1"))
                 .param("b", "x y")
                 .header("X-Test", "hello")
                 .cookie("sid", "42")
                 .cookie("lang", "fa")
                 .basic_auth("user", "pass")
                 .run_async();
    auto r = get(f);
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(has(r, "method=GET"));
    ASSERT_TRUE(has(r, "q=1,x y")); // params are encoded on the wire and decoded by the server
    ASSERT_TRUE(has(r, "x=hello"));
    ASSERT_TRUE(has(r, "cookie=sid=42; lang=fa"));
    ASSERT_TRUE(has(r, "auth=Basic dXNlcjpwYXNz")); // base64("user:pass")
    ASSERT_TRUE(has(r, "ua=httpp"));
}

UTEST(client_async, bearer_token_and_an_explicit_authorization_header) {
    testutil::local_server srv(add_echo);
    auto f1 = httpp::client::request(srv.url("/echo")).bearer("tok").run_async();
    ASSERT_TRUE(has(get(f1), "auth=Bearer tok"));
    auto f2 = httpp::client::request(srv.url("/echo")).bearer("tok").header("Authorization", "Custom z").run_async();
    ASSERT_TRUE(has(get(f2), "auth=Custom z")); // the caller's header wins
}

UTEST(client_async, body_methods_send_the_body_and_a_default_content_type) {
    testutil::local_server srv(add_echo);
    auto post = httpp::client::request(srv.url("/echo")).data("a=1&b=2").run_async();
    auto r = get(post);
    ASSERT_TRUE(has(r, "method=POST"));
    ASSERT_TRUE(has(r, "body=a=1&b=2"));
    ASSERT_TRUE(has(r, "ctype=application/x-www-form-urlencoded"));

    auto js = httpp::client::request(srv.url("/echo")).json("{\"k\":1}").run_async();
    auto rj = get(js);
    ASSERT_TRUE(has(rj, "ctype=application/json"));
    ASSERT_TRUE(has(rj, "body={\"k\":1}"));

    for (const char* m : {"PUT", "PATCH", "DELETE"}) {
        auto f = httpp::client::request(srv.url("/echo")).method(m).data("zz").run_async();
        auto rm = get(f);
        ASSERT_TRUE(has(rm, std::string("method=") + m));
        ASSERT_TRUE(has(rm, "body=zz"));
    }
    auto opt = httpp::client::request(srv.url("/echo")).method("OPTIONS").run_async();
    ASSERT_TRUE(has(get(opt), "method=OPTIONS"));
}

UTEST(client_async, head_has_headers_and_no_body) {
    testutil::local_server srv(add_echo);
    auto f = httpp::client::request(srv.url("/echo")).method("HEAD").run_async();
    auto r = get(f);
    ASSERT_EQ(200, r.status); // HEAD runs the GET route; the server drops the body but keeps its length
    ASSERT_TRUE(r.body.empty());
    bool has_length = false;
    for (const auto& [k, v] : r.headers) if (k == "Content-Length" && v != "0") has_length = true;
    ASSERT_TRUE(has_length);
}

UTEST(client_async, bad_input_comes_back_as_an_error_not_an_exception) {
    auto f1 = httpp::client::request("not a url").run_async();
    auto r1 = get(f1);
    ASSERT_EQ(0, r1.status);
    ASSERT_TRUE(r1.error == httpp::error_kind::invalid_url);
    auto f2 = httpp::client::request("http://127.0.0.1:1/").method("BREW").run_async();
    ASSERT_TRUE(get(f2).error == httpp::error_kind::unsupported_method);
    auto f3 = httpp::client::request("http://127.0.0.1:1/").timeout(2).run_async(); // nothing listens there
    ASSERT_TRUE(get(f3).error == httpp::error_kind::connection);
}

UTEST(client_async, timeout_is_reported_as_a_timeout) {
    testutil::local_server srv([](httpp::server& s) {
        s.get("/slow", [](const httpp::request&, httpp::response& r) { std::this_thread::sleep_for(3s); r.body = "late"; });
    });
    const auto t0 = std::chrono::steady_clock::now();
    auto f = httpp::client::request(srv.url("/slow")).timeout(1).run_async();
    auto r = get(f);
    ASSERT_TRUE(r.error == httpp::error_kind::timeout);
    ASSERT_TRUE(std::chrono::steady_clock::now() - t0 < 2500ms);
}

UTEST(client_async, max_response_size_gives_413_and_too_large) {
    testutil::local_server srv([](httpp::server& s) {
        s.get("/big", [](const httpp::request&, httpp::response& r) { r.body = std::string(100000, 'x'); });
    });
    auto f = httpp::client::request(srv.url("/big")).max_response_size(1000).run_async();
    auto r = get(f);
    ASSERT_EQ(413, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::too_large);
}

// ------------------------------------------------------------------ redirects

namespace {
void add_redirects(httpp::server& s) {
    s.get("/start", [](const httpp::request&, httpp::response& r) { r.status = 302; r.headers.emplace_back("Location", "/dir/mid"); });
    s.get("/dir/mid", [](const httpp::request&, httpp::response& r) { r.status = 301; r.headers.emplace_back("Location", "../end?x=1"); });
    s.get("/end", [](const httpp::request& q, httpp::response& r) { r.body = "end:" + q.query_param("x"); });
    s.post("/post303", [](const httpp::request&, httpp::response& r) { r.status = 303; r.headers.emplace_back("Location", "/echo"); });
    s.post("/post307", [](const httpp::request&, httpp::response& r) { r.status = 307; r.headers.emplace_back("Location", "/echo"); });
    s.get("/loop", [](const httpp::request&, httpp::response& r) { r.status = 302; r.headers.emplace_back("Location", "/loop"); });
    s.get("/nolocation", [](const httpp::request&, httpp::response& r) { r.status = 302; });
    add_echo(s);
}
} // namespace

UTEST(client_async_redirects, are_not_followed_by_default) {
    testutil::local_server srv(add_redirects);
    auto f = httpp::client::request(srv.url("/start")).run_async();
    auto r = get(f);
    ASSERT_EQ(302, r.status);
}

UTEST(client_async_redirects, relative_locations_are_resolved_hop_by_hop) {
    testutil::local_server srv(add_redirects);
    auto f = httpp::client::request(srv.url("/start")).follow_redirects(true).run_async();
    auto r = get(f);
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(r.body == "end:1"); // /start -> /dir/mid -> ../end?x=1 = /end?x=1
}

UTEST(client_async_redirects, a_303_turns_a_post_into_a_get_and_a_307_keeps_it) {
    testutil::local_server srv(add_redirects);
    auto f1 = httpp::client::request(srv.url("/post303")).data("payload").follow_redirects(true).run_async();
    auto r1 = get(f1);
    ASSERT_TRUE(has(r1, "method=GET"));
    ASSERT_TRUE(has(r1, "body="));
    auto f2 = httpp::client::request(srv.url("/post307")).data("payload").follow_redirects(true).run_async();
    auto r2 = get(f2);
    ASSERT_TRUE(has(r2, "method=POST"));
    ASSERT_TRUE(has(r2, "body=payload"));
}

UTEST(client_async_redirects, a_loop_ends_in_too_many_redirects) {
    testutil::local_server srv(add_redirects);
    auto f = httpp::client::request(srv.url("/loop")).follow_redirects(true).run_async();
    auto r = get(f);
    ASSERT_EQ(0, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::too_many_redirects);
}

UTEST(client_async_redirects, a_redirect_without_a_location_is_returned_as_it_is) {
    testutil::local_server srv(add_redirects);
    auto f = httpp::client::request(srv.url("/nolocation")).follow_redirects(true).run_async();
    ASSERT_EQ(302, get(f).status);
}

UTEST(client_async_redirects, credentials_stay_on_the_same_origin_and_are_dropped_across_origins) {
    testutil::local_server other(add_echo);
    testutil::local_server origin([&](httpp::server& s) {
        const std::string target = other.url("/echo"); // a different port: a different origin
        s.get("/to-other", [target](const httpp::request&, httpp::response& r) { r.status = 302; r.headers.emplace_back("Location", target); });
        s.get("/to-same", [](const httpp::request&, httpp::response& r) { r.status = 302; r.headers.emplace_back("Location", "/echo"); });
        add_echo(s);
    });
    auto same = httpp::client::request(origin.url("/to-same")).basic_auth("u", "p").cookie("c", "1").header("X-Test", "keep").follow_redirects(true).run_async();
    auto rs = get(same);
    ASSERT_TRUE(has(rs, "auth=Basic dTpw"));
    ASSERT_TRUE(has(rs, "cookie=c=1"));
    auto cross = httpp::client::request(origin.url("/to-other")).basic_auth("u", "p").cookie("c", "1").header("X-Test", "keep").follow_redirects(true).run_async();
    auto rc = get(cross);
    ASSERT_EQ(200, rc.status);
    ASSERT_TRUE(has(rc, "auth="));   // Authorization was not sent to the other origin
    ASSERT_TRUE(has(rc, "cookie=")); // nor was the Cookie
    ASSERT_TRUE(has(rc, "x=keep"));  // ordinary headers still are
}

// ------------------------------------------------------------------ many at once

UTEST(client_async, hundreds_of_requests_in_flight_at_once) {
    testutil::local_server srv([](httpp::server& s) {
        s.get("/n", [](const httpp::request& q, httpp::response& r) { r.body = q.query_param("i"); });
    });
    constexpr int N = 300;
    std::vector<std::future<httpp::response>> fs;
    for (int i = 0; i < N; ++i) fs.push_back(httpp::client::request(srv.url("/n?i=" + std::to_string(i))).run_async());
    int good = 0;
    for (int i = 0; i < N; ++i) {
        auto r = get(fs[i], 60);
        if (r.status == 200 && r.body == std::to_string(i)) ++good;
    }
    ASSERT_EQ(N, good);
}

// ------------------------------------------------------------------ async_client

UTEST(async_client_engine, applies_base_path_defaults_and_per_call_overrides) {
    testutil::local_server srv([](httpp::server& s) {
        s.get("/api/v1/echo", [](const httpp::request& q, httpp::response& r) {
            r.body = "x=" + q.header("X-Test") + ";y=" + q.header("X-Other") + ";auth=" + q.header("Authorization");
        });
    });
    httpp::async_client ac(srv.url("/api/v1/"), 2);
    ac.set_header("X-Test", "default").set_header("X-Other", "o").set_basic_auth("u", "p");
    auto f1 = ac.get("/echo");
    ASSERT_TRUE(get(f1).body == "x=default;y=o;auth=Basic dTpw");
    auto f2 = ac.get("echo", {{"X-Test", "override"}}); // no leading slash is fine; per-call header replaces the default
    ASSERT_TRUE(get(f2).body == "x=override;y=o;auth=Basic dTpw");
}

UTEST(async_client_engine, post_put_patch_delete_and_send) {
    testutil::local_server srv(add_echo);
    httpp::async_client ac(srv.url(), 4);
    auto p = ac.post("/echo", "body1", "text/plain");
    auto rp = get(p);
    ASSERT_TRUE(has(rp, "method=POST") && has(rp, "body=body1") && has(rp, "ctype=text/plain"));
    auto d = ac.del("/echo", "gone");
    ASSERT_TRUE(has(get(d), "method=DELETE"));
    auto s = ac.send("PATCH", "/echo", {}, "pp");
    ASSERT_TRUE(has(get(s), "method=PATCH"));
    auto bad = ac.send("BREW", "/echo");
    ASSERT_TRUE(get(bad).error == httpp::error_kind::unsupported_method);
}

UTEST(async_client_engine, at_most_max_connections_requests_run_at_a_time) {
    std::atomic<int> running{0}, peak{0};
    testutil::local_server srv([&](httpp::server& s) {
        s.get("/w", [&](const httpp::request&, httpp::response& r) {
            const int now = ++running;
            int p = peak.load();
            while (now > p && !peak.compare_exchange_weak(p, now)) {}
            std::this_thread::sleep_for(100ms);
            --running;
            r.body = "ok";
        });
    });
    httpp::async_client ac(srv.url(), 2);
    std::vector<std::future<httpp::response>> fs;
    for (int i = 0; i < 8; ++i) fs.push_back(ac.get("/w"));
    for (auto& f : fs) ASSERT_EQ(200, get(f).status);
    ASSERT_TRUE(peak.load() <= 2);
    ASSERT_TRUE(peak.load() >= 1);
}

UTEST(async_client_engine, follows_redirects_when_asked) {
    testutil::local_server srv(add_redirects);
    httpp::async_client ac(srv.url(), 2);
    auto first = ac.get("/start");
    ASSERT_EQ(302, get(first).status);
    ac.set_follow_redirects(true);
    auto f = ac.get("/start");
    ASSERT_TRUE(get(f).body == "end:1");
}

UTEST(async_client_engine, a_bad_base_url_throws_at_construction) {
    bool threw = false;
    try { httpp::async_client ac("nonsense"); } catch (const std::invalid_argument&) { threw = true; }
    ASSERT_TRUE(threw);
}

UTEST(async_client_engine, an_unreachable_server_is_an_error_response_and_the_slot_is_freed) {
    httpp::async_client ac("http://127.0.0.1:1", 1);
    ac.set_timeout(2);
    for (int i = 0; i < 3; ++i) { // with a leaked slot the second request would wait forever
        auto f = ac.get("/");
        ASSERT_TRUE(get(f, 15).error == httpp::error_kind::connection);
    }
}

// ------------------------------------------------------------------ https

UTEST(client_async, https_with_a_ca_file) {
    testtls::tls_server srv(testcerts::good_cert, testcerts::good_key, [](const std::string&) { return testtls::ok_response("secure"); });
    const std::string ca = testtls::write_pem(testcerts::good_cert, "good");
    auto f = httpp::client::request("https://127.0.0.1:" + std::to_string(srv.port())).ca_file(ca).run_async();
    auto r = get(f);
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(r.body == "secure");
    auto bad = httpp::client::request("https://127.0.0.1:" + std::to_string(srv.port())).run_async(); // system roots: self-signed
    ASSERT_TRUE(get(bad).error == httpp::error_kind::tls);
    auto off = httpp::client::request("https://127.0.0.1:" + std::to_string(srv.port())).verify(false).run_async();
    ASSERT_EQ(200, get(off).status);
}

// ------------------------------------------------------------------ callback forms

UTEST(client_async, run_async_with_a_callback_answers_once_from_the_network_thread) {
    testutil::local_server srv(add_echo);
    std::promise<std::pair<int, std::thread::id>> got;
    std::atomic<int> calls{0};
    httpp::client::request(srv.url("/echo")).run_async([&](httpp::response r) {
        ++calls;
        got.set_value({r.status, std::this_thread::get_id()});
    });
    auto fut = got.get_future();
    ASSERT_TRUE(fut.wait_for(10s) == std::future_status::ready);
    auto [status, tid] = fut.get();
    ASSERT_EQ(200, status);
    ASSERT_TRUE(tid != std::this_thread::get_id()); // not the caller's thread
    std::this_thread::sleep_for(50ms);
    ASSERT_EQ(1, calls.load());
}

UTEST(client_async, a_request_that_cannot_start_calls_back_at_once_on_the_calling_thread) {
    std::thread::id where;
    httpp::error_kind kind = httpp::error_kind::other;
    httpp::client::request("not a url").run_async([&](httpp::response r) {
        where = std::this_thread::get_id();
        kind = r.error;
    });
    ASSERT_TRUE(where == std::this_thread::get_id());
    ASSERT_TRUE(kind == httpp::error_kind::invalid_url);
}

UTEST(async_client_engine, send_with_a_callback_respects_the_connection_cap) {
    testutil::local_server srv(add_echo);
    httpp::async_client ac(srv.url(), 2);
    std::promise<std::string> got;
    ac.send("POST", "/echo", {}, "cb-body", "text/plain", [&](httpp::response r) { got.set_value(r.body); });
    auto fut = got.get_future();
    ASSERT_TRUE(fut.wait_for(10s) == std::future_status::ready);
    ASSERT_TRUE(fut.get().find("body=cb-body") != std::string::npos);
}
