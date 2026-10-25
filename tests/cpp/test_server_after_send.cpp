// response::after_send: background work that starts once the answer has been written, on a worker thread.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using testutil::local_server;

namespace {
bool wait_for(const std::atomic<int>& n, int want, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (n.load() < want) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}
}  // namespace

UTEST(httpp_server_after_send, the_client_gets_the_answer_before_the_background_job_finishes) {
    auto started = std::make_shared<std::atomic<int>>(0), finished = std::make_shared<std::atomic<int>>(0);
    local_server srv([&](httpp::server& s) {
        s.get("/", [=](const httpp::request&, httpp::response& res) {
            res.body = "ok";
            res.after_send = [=] {
                started->fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                finished->fetch_add(1);
            };
        });
    });
    const auto r = httpp::client::fetch(srv.url("/"));
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("ok", r.body.c_str());
    ASSERT_EQ(0, finished->load());          // the answer did not wait for the job
    ASSERT_TRUE(wait_for(*finished, 1));     // and the job does run
    ASSERT_EQ(1, started->load());
}

UTEST(httpp_server_after_send, it_runs_exactly_once_for_sync_async_and_streamed_answers) {
    auto n = std::make_shared<std::atomic<int>>(0);
    local_server srv([&](httpp::server& s) {
        s.get("/sync", [=](const httpp::request&, httpp::response& res) {
            res.body = "s";
            res.after_send = [=] { n->fetch_add(1); };
        });
        s.get_async("/async", [=](const httpp::request&, httpp::responder rp) {
            httpp::response res;
            res.body = "a";
            res.after_send = [=] { n->fetch_add(1); };
            rp.send(res);
        });
        s.get("/stream", [=](const httpp::request&, httpp::response& res) {
            res.stream = [i = 0](std::size_t, std::string& piece) mutable { piece = "x"; return ++i < 3; };
            res.after_send = [=] { n->fetch_add(1); };
        });
    });
    ASSERT_EQ(200, httpp::client::fetch(srv.url("/sync")).status);
    ASSERT_EQ(200, httpp::client::fetch(srv.url("/async")).status);
    ASSERT_STREQ("xxx", httpp::client::fetch(srv.url("/stream")).body.c_str());
    ASSERT_TRUE(wait_for(*n, 3));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_EQ(3, n->load());
}

UTEST(httpp_server_after_send, a_job_that_throws_does_not_hurt_the_server) {
    auto n = std::make_shared<std::atomic<int>>(0);
    local_server srv([&](httpp::server& s) {
        s.get("/", [=](const httpp::request&, httpp::response& res) {
            res.body = "ok";
            res.after_send = [=] { n->fetch_add(1); throw std::runtime_error("boom"); };
        });
    });
    for (int i = 0; i < 3; ++i) ASSERT_EQ(200, httpp::client::fetch(srv.url("/")).status);
    ASSERT_TRUE(wait_for(*n, 3));
}

UTEST(httpp_server_after_send, a_job_waits_for_its_own_answer_only_and_many_requests_each_run_theirs) {
    auto n = std::make_shared<std::atomic<int>>(0);
    local_server srv([&](httpp::server& s) {
        s.get("/", [=](const httpp::request&, httpp::response& res) {
            res.body = "ok";
            res.after_send = [=] { n->fetch_add(1); };
        });
    });
    httpp::client c(srv.url());  // one keep-alive connection
    for (int i = 0; i < 20; ++i) ASSERT_EQ(200, c.get("/").status);
    ASSERT_TRUE(wait_for(*n, 20));
}
