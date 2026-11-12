// The blocking client (client, httpp::request::run, client::fetch) runs on the same non-blocking
// engine and the same policy code as the async client (ADR-0051). These tests pin what that changes.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "local_server.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

void add_routes(httpp::server& s) {
    s.get("/echo", [](const httpp::server_request& q, httpp::response& r) {
        r.status = 200;
        r.body = "x=" + q.header("X-Test") + "\nauth=" + q.header("Authorization") + "\nq=" + q.query_param("n") + "\n";
    });
    s.get("/hop", [](const httpp::server_request&, httpp::response& r) {
        r.status = 302;
        r.headers.emplace_back("Location", "/echo?n=hopped");
    });
    s.get("/big", [](const httpp::server_request&, httpp::response& r) {
        r.status = 200;
        r.body = std::string(4096, 'z');
    });
}

bool has(const httpp::response& r, const std::string& line) { return r.body.find(line + "\n") != std::string::npos; }

} // namespace

// A blocking call made from the engine's own thread (inside a run_async callback) would wait for a loop
// that is waiting for it. It must fail fast with an error instead of hanging.
UTEST(client_blocking, call_from_the_network_thread_fails_instead_of_deadlocking) {
    testutil::local_server srv(add_routes);
    auto inner = std::make_shared<std::promise<httpp::response>>();
    auto inner_future = inner->get_future();
    auto outer = httpp::request(srv.url("/echo")).run_async();
    httpp::request(srv.url("/echo")).run_async([inner, url = srv.url("/echo")](httpp::response) {
        inner->set_value(httpp::client::fetch(url)); // on the network thread
    });
    ASSERT_TRUE(inner_future.wait_for(10s) == std::future_status::ready);
    const auto r = inner_future.get();
    ASSERT_EQ(0, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::other);
    ASSERT_TRUE(r.error_message.find("network thread") != std::string::npos);
    ASSERT_EQ(200, outer.get().status); // the engine itself is fine
}

// The session is not a bottleneck any more: threads may share one client and calls run side by side.
UTEST(client_blocking, one_client_shared_by_many_threads) {
    testutil::local_server srv(add_routes);
    httpp::client cli(srv.url());
    std::atomic<int> good{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&, i] {
            for (int k = 0; k < 10; ++k) {
                auto r = cli.get("/echo?n=" + std::to_string(i));
                if (r.status == 200 && has(r, "q=" + std::to_string(i))) ++good;
            }
        });
    for (auto& t : threads) t.join();
    ASSERT_EQ(80, good.load());
}

// Settings change while other threads are sending: a request uses the settings it started with.
UTEST(client_blocking, settings_apply_to_the_next_request) {
    testutil::local_server srv(add_routes);
    httpp::client cli(srv.url());
    cli.set_header("X-Test", "one");
    ASSERT_TRUE(has(cli.get("/echo"), "x=one"));
    cli.set_header("x-test", "two"); // same header, other case: replaced, not added
    ASSERT_TRUE(has(cli.get("/echo"), "x=two"));
    ASSERT_TRUE(has(cli.get("/echo", {{"X-Test", "call"}}), "x=call")); // per call beats the default
    cli.set_bearer_token("tok");
    ASSERT_TRUE(has(cli.get("/echo"), "auth=Bearer tok"));
    cli.set_basic_auth("user", "pass");
    ASSERT_TRUE(has(cli.get("/echo"), "auth=Basic dXNlcjpwYXNz"));
}

UTEST(client_blocking, redirects_are_followed_only_when_asked) {
    testutil::local_server srv(add_routes);
    httpp::client cli(srv.url());
    ASSERT_EQ(302, cli.get("/hop").status);
    cli.set_follow_redirects(true);
    auto r = cli.get("/hop");
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(has(r, "q=hopped"));
}

UTEST(client_blocking, request_run_honours_max_response_size) {
    testutil::local_server srv(add_routes);
    auto r = httpp::request(srv.url("/big")).max_response_size(100).run();
    ASSERT_EQ(413, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::too_large);
    ASSERT_EQ(200, httpp::request(srv.url("/big")).max_response_size(10000).run().status);
}

UTEST(client_blocking, unsupported_method_is_reported_before_any_network_use) {
    httpp::client cli("127.0.0.1", 1); // nothing listens there
    auto r = cli.send("BREW", "/");
    ASSERT_TRUE(r.error == httpp::error_kind::unsupported_method);
}

UTEST(client_blocking, async_client_set_header_replaces_a_same_name_default) {
    testutil::local_server srv(add_routes);
    httpp::async_client cli(srv.url());
    cli.set_header("X-Test", "one").set_header("x-test", "two");
    auto f = cli.get("/echo");
    ASSERT_TRUE(f.wait_for(10s) == std::future_status::ready);
    ASSERT_TRUE(has(f.get(), "x=two"));
}

UTEST(client_blocking, fetch_does_not_leave_the_client_in_a_bad_state) {
    testutil::local_server srv(add_routes);
    for (int i = 0; i < 3; ++i) ASSERT_EQ(200, httpp::client::fetch(srv.url("/echo?n=1")).status);
}
