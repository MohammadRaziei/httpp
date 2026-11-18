#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

// request::max_time: one limit for the whole request, redirects included (ADR-0084). timeout() still limits each step.

namespace {
double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
} // namespace

UTEST(httpp_client_max_time, a_slow_answer_ends_with_a_timeout_when_the_time_is_up) {
    testutil::local_server ls([](httpp::server& s) {
        s.get("/slow", [](const httpp::server_request&, httpp::response& res) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
            res.status = 200;
            res.body = "late";
        });
    });
    const auto start = std::chrono::steady_clock::now();
    httpp::response res = httpp::request(ls.url("/slow")).max_time(1).run();
    const double took = seconds_since(start);
    ASSERT_FALSE(res.ok());
    ASSERT_STREQ("timeout", res.error_name());
    ASSERT_GT(took, 0.8);
    ASSERT_LT(took, 2.5);
}

UTEST(httpp_client_max_time, the_per_step_timeout_alone_does_not_end_a_request_that_keeps_going) {
    testutil::local_server ls([](httpp::server& s) {
        s.get("/ok", [](const httpp::server_request&, httpp::response& res) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            res.status = 200;
            res.body = "fine";
        });
    });
    httpp::response res = httpp::request(ls.url("/ok")).timeout(10).max_time(5).run();
    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("fine", res.body.c_str());
}

UTEST(httpp_client_max_time, the_time_is_shared_by_all_the_hops_of_a_redirect_chain) {
    testutil::local_server ls([](httpp::server& s) {
        for (const char* from : {"/a", "/b", "/c"}) {
            const std::string name = from;
            s.get(name, [name](const httpp::server_request&, httpp::response& res) {
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                res.status = 302;
                res.headers.emplace_back("Location", name == "/a" ? "/b" : name == "/b" ? "/c" : "/d");
            });
        }
        s.get("/d", [](const httpp::server_request&, httpp::response& res) { res.status = 200; res.body = "end"; });
    });
    const auto start = std::chrono::steady_clock::now();
    httpp::response res = httpp::request(ls.url("/a")).max_redirects(5).max_time(1).run();
    ASSERT_FALSE(res.ok());
    ASSERT_STREQ("timeout", res.error_name());
    ASSERT_LT(seconds_since(start), 2.0); // not 3 hops of 0.7 s: the limit is for the whole chain
}

UTEST(httpp_client_max_time, zero_means_no_limit_and_a_negative_number_throws) {
    testutil::local_server ls([](httpp::server& s) {
        s.get("/ok", [](const httpp::server_request&, httpp::response& res) { res.status = 200; res.body = "x"; });
    });
    ASSERT_TRUE(httpp::request(ls.url("/ok")).max_time(0).run().ok());
    bool threw = false;
    try {
        httpp::request(ls.url("/ok")).max_time(-1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}
