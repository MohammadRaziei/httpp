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

// The response says how long the request ran and how many body bytes arrived, so a caller (the curl command, say) can tell a
// server that never answered from one that was still sending when the time ran out, without timing the call itself.

UTEST(httpp_client_max_time, a_timeout_reports_how_long_it_ran_and_how_much_arrived) {
    testutil::local_server ls([](httpp::server& s) {
        s.get("/drip", [](const httpp::server_request&, httpp::response& res) {
            res.status = 200;
            res.stream = [](std::size_t, std::string& chunk) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                chunk = "frame\n";
                return true; // never ends
            };
        });
    });
    std::size_t seen = 0;
    httpp::response res = httpp::request(ls.url("/drip"))
                              .max_time(1)
                              .on_body([&](std::string_view piece) { seen += piece.size(); return true; })
                              .run();
    ASSERT_STREQ("timeout", res.error_name());
    ASSERT_GT(res.elapsed_ms, 800);
    ASSERT_LT(res.elapsed_ms, 2500);
    ASSERT_GT(res.bytes_received, static_cast<std::size_t>(0));
    ASSERT_EQ(seen, res.bytes_received);
}

UTEST(httpp_client_max_time, a_timeout_before_any_byte_reports_zero_bytes) {
    testutil::local_server ls([](httpp::server& s) {
        s.get("/slow", [](const httpp::server_request&, httpp::response& res) {
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
            res.status = 200;
        });
    });
    httpp::response res = httpp::request(ls.url("/slow")).max_time(1).run();
    ASSERT_STREQ("timeout", res.error_name());
    ASSERT_GT(res.elapsed_ms, 800);
    ASSERT_EQ(res.bytes_received, static_cast<std::size_t>(0));
}

UTEST(httpp_client_max_time, a_collected_answer_counts_its_body_bytes_and_the_time_of_the_whole_chain) {
    testutil::local_server ls([](httpp::server& s) {
        s.get("/a", [](const httpp::server_request&, httpp::response& res) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            res.status = 302;
            res.headers.emplace_back("Location", "/b");
        });
        s.get("/b", [](const httpp::server_request&, httpp::response& res) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            res.status = 200;
            res.body = "twelve bytes";
        });
    });
    httpp::response res = httpp::request(ls.url("/a")).max_redirects(3).run();
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(res.bytes_received, res.body.size()); // the final answer only, not the redirect before it
    ASSERT_GT(res.elapsed_ms, 550);                  // both hops, from the first byte sent to the last received
}
