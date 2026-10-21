// route_io (runs on the I/O thread, for event loops that live elsewhere) and route_shared (a worker
// handler that gets its request by shared pointer).
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using testutil::local_server;

namespace {

void answer(httpp::responder& r, int status, const std::string& body) {
    httpp::response res;
    res.status = status;
    res.body = body;
    r.send(res);
}

std::string get(const local_server& srv, const std::string& path) {
    return httpp::client::fetch(srv.url(path)).body;
}

} // namespace

UTEST(httpp_server_io, an_io_route_runs_on_the_listening_thread_and_a_normal_route_does_not) {
    std::mutex mu;
    std::thread::id io_id, worker_id;
    local_server srv([&](httpp::server& s) {
        s.route_io("GET", "/io", [&](std::shared_ptr<const httpp::request>, httpp::responder r) {
            {
                std::lock_guard<std::mutex> lock(mu);
                io_id = std::this_thread::get_id();
            }
            answer(r, 200, "io");
        });
        s.get("/worker", [&](const httpp::request&, httpp::response& res) {
            {
                std::lock_guard<std::mutex> lock(mu);
                worker_id = std::this_thread::get_id();
            }
            res.body = "worker";
        });
    });
    ASSERT_STREQ("io", get(srv, "/io").c_str());
    ASSERT_STREQ("worker", get(srv, "/worker").c_str());
    std::lock_guard<std::mutex> lock(mu);
    ASSERT_TRUE(io_id == srv.th.get_id()); // the thread that called listen_after_bind()
    ASSERT_TRUE(worker_id != srv.th.get_id());
}

UTEST(httpp_server_io, an_io_route_sees_path_params_query_headers_and_body) {
    local_server srv([](httpp::server& s) {
        s.route_io("POST", "/io/:id/:name", [](std::shared_ptr<const httpp::request> req, httpp::responder r) {
            answer(r, 201,
                   "id=" + req->path_param("id") + ";name=" + req->path_param("name") + ";q=" + req->query_param("q") +
                       ";h=" + req->header("X-Test") + ";body=" + req->body + ";path=" + req->path + ";m=" + req->method);
        });
    });
    auto res = httpp::client::request(srv.url("/io/7/caf%C3%A9?q=a%20b")).method("POST").header("X-Test", "yes").data("xyz").run();
    ASSERT_EQ(201, res.status);
    ASSERT_STREQ("id=7;name=caf\xC3\xA9;q=a b;h=yes;body=xyz;path=/io/7/caf\xC3\xA9;m=POST", res.body.c_str());
}

UTEST(httpp_server_io, the_request_outlives_the_handler_and_the_answer_can_come_later_from_another_thread) {
    std::shared_ptr<const httpp::request> kept;
    httpp::responder later;
    std::mutex mu;
    std::thread th;
    {
        local_server srv([&](httpp::server& s) {
            s.route_io("GET", "/keep/:id", [&](std::shared_ptr<const httpp::request> req, httpp::responder r) {
                std::lock_guard<std::mutex> lock(mu);
                kept = req;
                later = r;
                th = std::thread([req, r]() mutable { // the request is still valid on another thread, later
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    answer(r, 200, "late " + req->path_param("id"));
                });
            });
        });
        ASSERT_STREQ("late 5", get(srv, "/keep/5").c_str());
    }
    th.join();
    std::lock_guard<std::mutex> lock(mu);
    ASSERT_TRUE(kept != nullptr);
    ASSERT_STREQ("/keep/5", kept->path.c_str()); // still alive after the server is gone
}

UTEST(httpp_server_io, an_io_route_answers_head_like_get_and_unknown_paths_and_methods_get_404) {
    local_server srv([](httpp::server& s) {
        s.route_io("GET", "/io", [](std::shared_ptr<const httpp::request>, httpp::responder r) { answer(r, 200, "body"); });
    });
    ASSERT_EQ(200, httpp::client::request(srv.url("/io")).method("HEAD").run().status);
    ASSERT_EQ(404, httpp::client::fetch(srv.url("/nope")).status);
    ASSERT_EQ(405, httpp::client::request(srv.url("/io")).method("DELETE").run().status); // the path exists, the method does not
}

UTEST(httpp_server_io, a_before_hook_still_runs_first_and_can_answer_in_front_of_an_io_route) {
    std::atomic<int> before_calls{0};
    local_server srv([&](httpp::server& s) {
        s.route_io("GET", "/io", [](std::shared_ptr<const httpp::request>, httpp::responder r) { answer(r, 200, "io"); });
        s.before([&](const httpp::request& req, httpp::response& res) {
            before_calls++;
            if (req.query_param("deny") == "1") {
                res.status = 403;
                res.body = "denied";
                return true;
            }
            return false;
        });
    });
    ASSERT_STREQ("io", get(srv, "/io").c_str());
    auto denied = httpp::client::fetch(srv.url("/io?deny=1"));
    ASSERT_EQ(403, denied.status);
    ASSERT_STREQ("denied", denied.body.c_str());
    ASSERT_EQ(2, before_calls.load());
}

UTEST(httpp_server_io, the_after_and_error_hooks_see_io_routes_and_the_404_of_an_unknown_path) {
    std::atomic<int> after_calls{0};
    local_server srv([&](httpp::server& s) {
        s.route_io("GET", "/io", [](std::shared_ptr<const httpp::request>, httpp::responder r) { answer(r, 200, "io"); });
        s.after([&](const httpp::request&, httpp::response& res) {
            after_calls++;
            res.headers.emplace_back("X-After", "1");
        });
        s.set_error_handler([](const httpp::request&, httpp::response& res) { res.body = "custom " + std::to_string(res.status); });
    });
    auto ok = httpp::client::fetch(srv.url("/io"));
    ASSERT_STREQ("1", ok.header("X-After").c_str());
    auto missing = httpp::client::fetch(srv.url("/nope"));
    ASSERT_EQ(404, missing.status);
    ASSERT_STREQ("custom 404", missing.body.c_str());
    ASSERT_STREQ("1", missing.header("X-After").c_str());
    ASSERT_EQ(2, after_calls.load());
}

UTEST(httpp_server_io, a_mounted_directory_still_wins_over_an_io_route_and_a_missing_file_falls_through) {
    const auto dir = std::filesystem::temp_directory_path() / "httpp_io_static";
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / "a.txt") << "from disk"; }
    local_server srv([&](httpp::server& s) {
        s.serve_directory("/files", dir.string());
        s.route_io("GET", "/files/:name", [](std::shared_ptr<const httpp::request> req, httpp::responder r) {
            answer(r, 200, "route " + req->path_param("name"));
        });
        s.route_io("GET", "/other", [](std::shared_ptr<const httpp::request>, httpp::responder r) { answer(r, 200, "other"); });
    });
    ASSERT_STREQ("from disk", get(srv, "/files/a.txt").c_str());
    ASSERT_STREQ("route b.txt", get(srv, "/files/b.txt").c_str()); // not on disk: the route answers
    ASSERT_STREQ("other", get(srv, "/other").c_str());
    std::filesystem::remove_all(dir);
}

UTEST(httpp_server_io, a_throwing_io_handler_becomes_500_or_goes_to_the_exception_handler) {
    {
        local_server srv([](httpp::server& s) {
            s.route_io("GET", "/boom", [](std::shared_ptr<const httpp::request>, httpp::responder) { throw std::runtime_error("secret"); });
        });
        auto res = httpp::client::fetch(srv.url("/boom"));
        ASSERT_EQ(500, res.status);
        ASSERT_TRUE(res.body.find("secret") == std::string::npos);
    }
    local_server srv([](httpp::server& s) {
        s.route_io("GET", "/boom", [](std::shared_ptr<const httpp::request>, httpp::responder) { throw std::runtime_error("secret"); });
        s.set_exception_handler([](const httpp::request&, httpp::response& res, std::exception_ptr) {
            res.status = 418;
            res.body = "handled";
        });
    });
    auto res = httpp::client::fetch(srv.url("/boom"));
    ASSERT_EQ(418, res.status);
    ASSERT_STREQ("handled", res.body.c_str());
}

UTEST(httpp_server_io, an_unanswered_io_route_gets_a_504) {
    auto stash = std::make_shared<httpp::responder>();
    local_server srv([&](httpp::server& s) {
        s.set_async_timeout(1);
        s.route_io("GET", "/never", [stash](std::shared_ptr<const httpp::request>, httpp::responder r) { *stash = r; });
    });
    auto res = httpp::client::request(srv.url("/never")).timeout(10).run();
    ASSERT_EQ(504, res.status);
}

UTEST(httpp_server_io, many_pipelined_style_requests_on_one_connection_all_get_their_own_answer) {
    local_server srv([](httpp::server& s) {
        s.route_io("GET", "/n/:i", [](std::shared_ptr<const httpp::request> req, httpp::responder r) {
            answer(r, 200, "n" + req->path_param("i"));
        });
    });
    for (int i = 0; i < 50; ++i) ASSERT_STREQ(("n" + std::to_string(i)).c_str(), get(srv, "/n/" + std::to_string(i)).c_str());
}

UTEST(httpp_server_shared, a_shared_route_runs_on_a_worker_and_may_keep_its_request) {
    std::mutex mu;
    std::thread::id worker_id;
    std::shared_ptr<const httpp::request> kept;
    local_server srv([&](httpp::server& s) {
        s.route_shared("PUT", "/s/:id", [&](std::shared_ptr<const httpp::request> req, httpp::response& res) {
            std::lock_guard<std::mutex> lock(mu);
            worker_id = std::this_thread::get_id();
            kept = req;
            res.status = 202;
            res.body = "s" + req->path_param("id") + req->body;
        });
    });
    auto res = httpp::client::request(srv.url("/s/9")).method("PUT").data("!").run();
    ASSERT_EQ(202, res.status);
    ASSERT_STREQ("s9!", res.body.c_str());
    std::lock_guard<std::mutex> lock(mu);
    ASSERT_TRUE(worker_id != srv.th.get_id());
    ASSERT_STREQ("9", kept->path_param("id").c_str());
}

UTEST(httpp_server_shared, a_throwing_shared_route_becomes_500) {
    local_server srv([](httpp::server& s) {
        s.route_shared("GET", "/boom", [](std::shared_ptr<const httpp::request>, httpp::response&) { throw std::runtime_error("secret"); });
    });
    ASSERT_EQ(500, httpp::client::fetch(srv.url("/boom")).status);
}

UTEST(httpp_server_io, an_unsupported_method_is_rejected_when_registering) {
    httpp::server s;
    bool threw = false;
    try {
        s.route_io("BREW", "/x", [](std::shared_ptr<const httpp::request>, httpp::responder) {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

// ---- the 504 backstop: one timer watches every waiting request ----------------------------------

UTEST(httpp_server_io, requests_that_wait_for_an_answer_are_each_timed_out_on_their_own_schedule) {
    std::mutex mu;
    std::vector<httpp::responder> kept;
    local_server srv([&](httpp::server& s) {
        s.set_async_timeout(1);
        s.route_io("GET", "/hold", [&](std::shared_ptr<const httpp::request>, httpp::responder r) {
            std::lock_guard<std::mutex> lock(mu);
            kept.push_back(r);
        });
        s.route_io("GET", "/quick", [](std::shared_ptr<const httpp::request>, httpp::responder r) { answer(r, 200, "q"); });
    });
    // five held requests, started 150 ms apart, all in flight together
    std::vector<std::thread> clients;
    std::vector<double> seconds(5, 0.0);
    std::vector<int> status(5, 0);
    for (int i = 0; i < 5; ++i) {
        clients.emplace_back([&, i] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150 * i));
            auto t0 = std::chrono::steady_clock::now();
            status[i] = httpp::client::request(srv.url("/hold")).timeout(10).run().status;
            seconds[i] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        });
    }
    // answered requests must not be touched by the timer later
    for (int i = 0; i < 20; ++i) ASSERT_STREQ("q", get(srv, "/quick").c_str());
    for (auto& c : clients) c.join();
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(504, status[i]);
        ASSERT_GE(seconds[i], 0.9);
        ASSERT_LT(seconds[i], 2.5);
    }
    ASSERT_STREQ("q", get(srv, "/quick").c_str()); // the server is still fine after the sweeps
}

UTEST(httpp_server_io, a_late_send_after_the_504_is_ignored_and_a_dropped_responder_closes_the_connection) {
    auto stash = std::make_shared<httpp::responder>();
    local_server srv([&](httpp::server& s) {
        s.set_async_timeout(1);
        s.route_io("GET", "/late", [stash](std::shared_ptr<const httpp::request>, httpp::responder r) { *stash = r; });
        s.route_io("GET", "/drop", [](std::shared_ptr<const httpp::request>, httpp::responder) { /* every copy dropped */ });
    });
    ASSERT_EQ(504, httpp::client::request(srv.url("/late")).timeout(10).run().status);
    httpp::response res;
    res.status = 200;
    res.body = "too late";
    stash->send(res); // must not crash or corrupt the next request
    auto t0 = std::chrono::steady_clock::now();
    auto dropped = httpp::client::request(srv.url("/drop")).timeout(10).run();
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_TRUE(dropped.status != 200);
    ASSERT_LT(seconds, 0.9); // closed at once, it did not wait for the timeout
}

UTEST(httpp_server_io, a_server_can_listen_again_and_the_timeout_still_works) {
    httpp::server s;
    s.set_async_timeout(1);
    auto stash = std::make_shared<httpp::responder>(); // keeps the request open (dropping it would close the connection)
    s.route_io("GET", "/never", [stash](std::shared_ptr<const httpp::request>, httpp::responder r) { *stash = r; });
    int statuses[2] = {0, 0};
    for (int round = 0; round < 2; ++round) {
        const int port = s.bind_to_any_port("127.0.0.1");
        std::thread th([&] { s.listen_after_bind(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        statuses[round] = httpp::client::request("http://127.0.0.1:" + std::to_string(port) + "/never").timeout(10).run().status;
        s.stop();
        th.join(); // joined before any assert, so a failure reports instead of terminating
    }
    ASSERT_EQ(504, statuses[0]);
    ASSERT_EQ(504, statuses[1]);
}
