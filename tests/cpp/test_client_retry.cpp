#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

// retry(), retry_delay(), retry_connrefused() and on_retry(): a request that failed for a reason that may pass is sent again
// by the library, not by its caller (ADR-0097). Only a timeout, a 408, 429, 500, 502, 503 or 504 and, if asked, a failed
// connection count, as in curl. retry_delay() is in whole seconds, so a test that uses it waits at least one; retry_delay_ms()
// is for the ones that must not.

namespace {
double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// 503 for the first `bad` calls, then 200 "fine".
void flaky_route(httpp::server& s, std::atomic<int>& hits, int bad, int bad_status = 503) {
    s.get("/flaky", [&hits, bad, bad_status](const httpp::server_request&, httpp::response& res) {
        if (hits++ < bad) {
            res.status = bad_status;
            res.body = "oops";
        } else {
            res.status = 200;
            res.body = "fine";
        }
    });
}
} // namespace

UTEST(httpp_client_retry, a_transient_status_is_sent_again_until_it_passes) {
    std::atomic<int> hits{0};
    testutil::local_server ls([&](httpp::server& s) { flaky_route(s, hits, 1); });
    const auto start = std::chrono::steady_clock::now();
    httpp::response res = httpp::request(ls.url("/flaky")).retry(2).retry_delay(1).run();
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("fine", res.body.c_str());
    ASSERT_EQ(2, hits.load());
    ASSERT_GT(seconds_since(start), 0.9); // it waited the delay
}

UTEST(httpp_client_retry, it_gives_up_after_the_given_number_of_retries_and_returns_the_last_answer) {
    std::atomic<int> hits{0};
    testutil::local_server ls([&](httpp::server& s) { flaky_route(s, hits, 100); });
    httpp::response res = httpp::request(ls.url("/flaky")).retry(1).retry_delay(1).run();
    ASSERT_EQ(503, res.status);
    ASSERT_STREQ("oops", res.body.c_str());
    ASSERT_EQ(2, hits.load()); // the first try and one retry
}

UTEST(httpp_client_retry, nothing_is_retried_by_default_and_a_404_never_is) {
    std::atomic<int> hits{0};
    testutil::local_server ls([&](httpp::server& s) {
        flaky_route(s, hits, 100);
        s.get("/gone", [&hits](const httpp::server_request&, httpp::response& res) { ++hits; res.status = 404; });
    });
    ASSERT_EQ(503, httpp::request(ls.url("/flaky")).run().status);
    ASSERT_EQ(1, hits.load());
    hits = 0;
    ASSERT_EQ(404, httpp::request(ls.url("/gone")).retry(3).retry_delay(1).run().status);
    ASSERT_EQ(1, hits.load());
}

UTEST(httpp_client_retry, a_timeout_is_retried) {
    std::atomic<int> hits{0};
    testutil::local_server ls([&](httpp::server& s) {
        s.get("/sometimes", [&hits](const httpp::server_request&, httpp::response& res) {
            if (hits++ == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            res.status = 200;
            res.body = "quick";
        });
    });
    httpp::response res = httpp::request(ls.url("/sometimes")).max_time(1).retry(1).retry_delay(1).run();
    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("quick", res.body.c_str());
    ASSERT_EQ(2, hits.load());
    ASSERT_LT(res.elapsed_ms, 900); // elapsed_ms and bytes_received are those of the last try, not of the waits
}

UTEST(httpp_client_retry, a_wait_in_milliseconds_is_not_rounded_up_to_a_second) {
    std::atomic<int> hits{0};
    testutil::local_server ls([&](httpp::server& s) { flaky_route(s, hits, 1); });
    const auto start = std::chrono::steady_clock::now();
    httpp::response res = httpp::request(ls.url("/flaky")).retry(2).retry_delay_ms(150).run();
    const double took = seconds_since(start);
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(2, hits.load());
    ASSERT_GT(took, 0.14); // it waited
    ASSERT_LT(took, 0.9);  // and not a whole second
}

UTEST(httpp_client_retry, a_failed_connection_is_retried_only_when_asked) {
    std::string dead;
    {
        testutil::local_server gone([](httpp::server&) {});
        dead = gone.url("/x"); // nobody listens there any more
    }
    int notices = 0;
    httpp::response plain = httpp::request(dead).retry(1).retry_delay(1).on_retry([&](const httpp::response&, int, long) { ++notices; }).run();
    ASSERT_TRUE(plain.error == httpp::error_kind::connection);
    ASSERT_EQ(0, notices);
    httpp::response asked =
        httpp::request(dead).retry(1).retry_delay(1).retry_connrefused(true).on_retry([&](const httpp::response&, int, long) { ++notices; }).run();
    ASSERT_TRUE(asked.error == httpp::error_kind::connection);
    ASSERT_EQ(1, notices);
}

UTEST(httpp_client_retry, on_retry_is_told_the_last_answer_the_number_of_the_retry_and_the_wait_before_it_starts) {
    std::atomic<int> hits{0};
    testutil::local_server ls([&](httpp::server& s) { flaky_route(s, hits, 1, 429); });
    int status = 0, number = 0;
    long delay = 0;
    int calls = 0;
    httpp::response res = httpp::request(ls.url("/flaky"))
                              .retry(3)
                              .on_retry([&](const httpp::response& last, int n, long wait) { // no retry_delay(): 1, 2, 4, ... seconds
                                  ++calls;
                                  status = last.status;
                                  number = n;
                                  delay = wait;
                              })
                              .run();
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(1, calls);
    ASSERT_EQ(429, status);
    ASSERT_EQ(1, number);
    ASSERT_EQ(1, delay);
}

UTEST(httpp_client_retry, on_retry_is_told_whole_seconds_rounded_up_for_a_wait_in_milliseconds) {
    std::atomic<int> hits{0};
    testutil::local_server ls([&](httpp::server& s) { flaky_route(s, hits, 1); });
    long delay = -1;
    httpp::response res =
        httpp::request(ls.url("/flaky")).retry(1).retry_delay_ms(100).on_retry([&](const httpp::response&, int, long wait) { delay = wait; }).run();
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(1, delay);
}

// With on_body() the body goes to the caller as it arrives, so an answer that is about to be sent again must never reach it.

UTEST(httpp_client_retry, a_streamed_answer_that_is_retried_is_never_shown_to_the_callbacks) {
    std::atomic<int> hits{0};
    testutil::local_server ls([&](httpp::server& s) { flaky_route(s, hits, 1); });
    std::string got;
    int heads = 0, head_status = 0;
    httpp::response res = httpp::request(ls.url("/flaky"))
                              .retry(1)
                              .retry_delay(1)
                              .on_head([&](const httpp::response& head) { ++heads; head_status = head.status; return true; })
                              .on_body([&](std::string_view piece) { got.append(piece); return true; })
                              .run();
    ASSERT_EQ(200, res.status);
    ASSERT_EQ(1, heads);
    ASSERT_EQ(200, head_status);
    ASSERT_STREQ("fine", got.c_str());
    ASSERT_EQ(static_cast<std::size_t>(4), res.bytes_received);
}

UTEST(httpp_client_retry, the_last_answer_of_a_streamed_request_is_shown_when_the_retries_run_out) {
    std::atomic<int> hits{0};
    testutil::local_server ls([&](httpp::server& s) { flaky_route(s, hits, 100); });
    std::string got;
    int heads = 0, head_status = 0;
    httpp::response res = httpp::request(ls.url("/flaky"))
                              .retry(1)
                              .retry_delay(1)
                              .on_head([&](const httpp::response& head) { ++heads; head_status = head.status; return true; })
                              .on_body([&](std::string_view piece) { got.append(piece); return true; })
                              .run();
    ASSERT_EQ(503, res.status);
    ASSERT_EQ(1, heads);
    ASSERT_EQ(503, head_status);
    ASSERT_STREQ("oops", got.c_str());
}

// The wait between two tries is a timer on the network thread, not a sleep on it: other requests go on meanwhile.

UTEST(httpp_client_retry, waiting_between_tries_does_not_hold_up_the_network_thread) {
    std::atomic<int> hits{0};
    testutil::local_server ls([&](httpp::server& s) {
        flaky_route(s, hits, 1);
        s.get("/ok", [](const httpp::server_request&, httpp::response& res) { res.status = 200; res.body = "x"; });
    });
    std::future<httpp::response> later = httpp::request(ls.url("/flaky")).retry(1).retry_delay(1).run_async();
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // the first try has failed, the second waits
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(httpp::request(ls.url("/ok")).run().ok());
    ASSERT_LT(seconds_since(start), 0.5);
    httpp::response res = later.get();
    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("fine", res.body.c_str());
}

UTEST(httpp_client_retry, an_async_client_retries_too) {
    std::atomic<int> hits{0};
    testutil::local_server ls([&](httpp::server& s) { flaky_route(s, hits, 1); });
    httpp::async_client ac{httpp::url(ls.url())};
    ac.retry(1).retry_delay(1);
    httpp::response res = ac.get("/flaky").get();
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(2, hits.load());
}

UTEST(httpp_client_retry, retry_delay_and_retry_delay_ms_are_one_setting_and_are_checked) {
    httpp::request r("http://127.0.0.1:1/x");
    ASSERT_EQ(0, r.retry_delay_ms());
    r.retry_delay_ms(250);
    ASSERT_EQ(250, r.retry_delay_ms());
    ASSERT_EQ(0, r.retry_delay()); // whole seconds, rounded down
    r.retry_delay(2);
    ASSERT_EQ(2000, r.retry_delay_ms()); // the last one set wins
    ASSERT_EQ(2, r.retry_delay());
    r.retry_delay_ms(0);
    ASSERT_EQ(0, r.retry_delay()); // 0 is the default back-off again

    httpp::client c{httpp::url("http://127.0.0.1:1")};
    c.retry_delay_ms(300);
    ASSERT_EQ(300, c.make_request("/x").retry_delay_ms()); // a session hands it on

    bool negative = false, too_long = false;
    try {
        r.retry_delay_ms(-1);
    } catch (const std::invalid_argument&) {
        negative = true;
    }
    try {
        r.retry_delay_ms(86400L * 1000 + 1);
    } catch (const std::invalid_argument&) {
        too_long = true;
    }
    ASSERT_TRUE(negative);
    ASSERT_TRUE(too_long);
}

UTEST(httpp_client_retry, the_settings_are_read_back_a_session_hands_them_on_and_a_negative_number_throws) {
    httpp::request plain("http://127.0.0.1:1/x");
    ASSERT_EQ(0, plain.retry());
    ASSERT_EQ(0, plain.retry_delay());
    ASSERT_FALSE(plain.retry_connrefused());

    httpp::client c{httpp::url("http://127.0.0.1:1")};
    c.retry(3).retry_delay(2).retry_connrefused(true);
    auto r = c.make_request("/x");
    ASSERT_EQ(3, r.retry());
    ASSERT_EQ(2, r.retry_delay());
    ASSERT_TRUE(r.retry_connrefused());

    httpp::async_client ac{httpp::url("http://127.0.0.1:1")};
    ac.retry(5);
    ASSERT_EQ(5, ac.retry());

    bool retry_threw = false, delay_threw = false;
    try {
        plain.retry(-1);
    } catch (const std::invalid_argument&) {
        retry_threw = true;
    }
    try {
        plain.retry_delay(-1);
    } catch (const std::invalid_argument&) {
        delay_threw = true;
    }
    ASSERT_TRUE(retry_threw);
    ASSERT_TRUE(delay_threw);
}
