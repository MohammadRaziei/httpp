// Server features: request helpers, settings, hooks, async timeout, file/stream responses.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using testutil::local_server;

UTEST(httpp_server_request, helpers_find_headers_query_path_params_and_cookies) {
    httpp::request req;
    req.headers = {{"X-Token", "abc"}, {"Cookie", "a=1; session=xyz; b=2"}};
    req.query = {{"page", "2"}};
    req.path_params = {{"id", "42"}};
    ASSERT_STREQ("abc", req.header("x-token").c_str()); // case-insensitive
    ASSERT_STREQ("", req.header("missing").c_str());
    ASSERT_STREQ("2", req.query_param("page").c_str());
    ASSERT_STREQ("42", req.path_param("id").c_str());
    ASSERT_STREQ("xyz", req.cookie("session").c_str());
    ASSERT_STREQ("2", req.cookie("b").c_str());
    ASSERT_STREQ("", req.cookie("nope").c_str());
}

UTEST(httpp_server_response, redirect_cookie_and_json_helpers) {
    local_server srv([](httpp::server& s) {
        s.get("/go", [](const httpp::request&, httpp::response& res) { res.redirect("/there", 301); });
        s.get("/c", [](const httpp::request&, httpp::response& res) {
            res.set_cookie("a", "1").set_cookie("b", "2", "Path=/api");
            res.body = "ok";
        });
        s.get("/j", [](const httpp::request&, httpp::response& res) { res.json("{\"a\":1}"); });
    });
    auto go = httpp::client::fetch(srv.url("/go"));
    ASSERT_EQ(301, go.status);
    ASSERT_STREQ("/there", go.header("Location").c_str());
    auto c = httpp::client::fetch(srv.url("/c"));
    int cookies = 0;
    for (const auto& [k, v] : c.headers) cookies += (k == "Set-Cookie");
    ASSERT_EQ(2, cookies);
    ASSERT_STREQ("application/json", httpp::client::fetch(srv.url("/j")).header("Content-Type").c_str());
}

UTEST(httpp_server_async, an_unanswered_route_gets_a_504_and_a_late_send_is_ignored) {
    auto stash = std::make_shared<httpp::responder>();
    std::atomic<bool> stored{false};
    local_server srv([&](httpp::server& s) {
        s.set_async_timeout(1);
        s.get_async("/never", [&](const httpp::request&, httpp::responder r) {
            *stash = r;
            stored = true;
        });
    });
    auto t0 = std::chrono::steady_clock::now();
    auto res = httpp::client::request(srv.url("/never")).timeout(10).run();
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    ASSERT_EQ(504, res.status);
    ASSERT_GE(seconds, 0.9);
    ASSERT_LT(seconds, 3.0);
    ASSERT_TRUE(stored.load());
    httpp::response late;
    late.status = 200;
    stash->send(late); // the connection was already answered: must be a harmless no-op
}

UTEST(httpp_server_async, an_answer_in_time_cancels_nothing_and_shorthands_register_the_right_method) {
    local_server srv([](httpp::server& s) {
        s.set_async_timeout(30);
        auto echo = [](const httpp::request& req, httpp::responder r) {
            httpp::response res;
            res.status = 200;
            res.body = req.method;
            r.send(res);
        };
        s.get_async("/m", echo);
        s.post_async("/m", echo);
        s.put_async("/m", echo);
        s.patch_async("/m", echo);
        s.del_async("/m", echo);
    });
    for (const char* method : {"GET", "POST", "PUT", "PATCH", "DELETE"}) {
        auto res = httpp::client::request(srv.url("/m")).method(method).run();
        ASSERT_EQ(200, res.status);
        ASSERT_STREQ(method, res.body.c_str());
    }
}

UTEST(httpp_server_hooks, before_can_reject_and_after_adds_headers_to_sync_and_async_responses) {
    local_server srv([](httpp::server& s) {
        s.before([](const httpp::request& req, httpp::response& res) {
            if (req.header("X-Key") != "secret" && req.path != "/open") {
                res.status = 401;
                res.body = "denied";
                return true;
            }
            return false;
        });
        s.after([](const httpp::request&, httpp::response& res) { res.headers.emplace_back("X-After", "yes"); });
        s.get("/sync", [](const httpp::request&, httpp::response& res) { res.body = "s"; });
        s.get_async("/async", [](const httpp::request&, httpp::responder r) {
            httpp::response res;
            res.status = 200;
            res.body = "a";
            r.send(res);
        });
        s.get("/open", [](const httpp::request&, httpp::response& res) { res.body = "o"; });
    });
    auto denied = httpp::client::fetch(srv.url("/sync"));
    ASSERT_EQ(401, denied.status);
    ASSERT_STREQ("denied", denied.body.c_str());
    ASSERT_STREQ("yes", denied.header("X-After").c_str()); // after-hooks see rejected requests too

    for (const char* path : {"/sync", "/async"}) {
        auto res = httpp::client::request(srv.url(path)).header("X-Key", "secret").run();
        ASSERT_EQ(200, res.status);
        ASSERT_STREQ("yes", res.header("X-After").c_str());
    }
    ASSERT_EQ(200, httpp::client::fetch(srv.url("/open")).status);
}

UTEST(httpp_server_hooks, error_and_exception_handlers_shape_the_response) {
    local_server srv([](httpp::server& s) {
        s.set_error_handler([](const httpp::request& req, httpp::response& res) {
            res.body = "custom " + std::to_string(res.status) + " for " + req.path;
        });
        s.set_exception_handler([](const httpp::request&, httpp::response& res, std::exception_ptr ep) {
            try { std::rethrow_exception(ep); }
            catch (const std::runtime_error& e) { res.status = 418; res.body = std::string("teapot: ") + e.what(); }
            catch (...) {}
        });
        s.get("/boom", [](const httpp::request&, httpp::response&) { throw std::runtime_error("sync"); });
        s.get_async("/aboom", [](const httpp::request&, httpp::responder) { throw std::runtime_error("async"); });
    });
    auto nf = httpp::client::fetch(srv.url("/missing"));
    ASSERT_EQ(404, nf.status);
    ASSERT_STREQ("custom 404 for /missing", nf.body.c_str());
    auto boom = httpp::client::fetch(srv.url("/boom"));
    ASSERT_EQ(418, boom.status);
    ASSERT_STREQ("teapot: sync", boom.body.c_str());
    auto aboom = httpp::client::fetch(srv.url("/aboom"));
    ASSERT_EQ(418, aboom.status); // a throwing async handler goes through the same exception handler
    ASSERT_STREQ("teapot: async", aboom.body.c_str());
}

UTEST(httpp_server_response, a_file_is_streamed_from_disk_in_sync_and_async_routes) {
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "httpp_file_response_test.bin";
    std::string content(3'000'000, 'q');
    content[1234567] = 'X';
    { std::ofstream(path, std::ios::binary) << content; }
    local_server srv([&](httpp::server& s) {
        s.get("/f", [&](const httpp::request&, httpp::response& res) {
            res.file = path.string();
            res.headers.emplace_back("Content-Type", "application/octet-stream");
        });
        s.get_async("/af", [&](const httpp::request&, httpp::responder r) {
            httpp::response res;
            res.file = path.string();
            r.send(res);
        });
        s.get("/missing", [](const httpp::request&, httpp::response& res) { res.file = "/no/such/file"; });
    });
    for (const char* route : {"/f", "/af"}) {
        auto res = httpp::client::fetch(srv.url(route));
        ASSERT_EQ(200, res.status);
        ASSERT_EQ(content.size(), res.body.size());
        ASSERT_TRUE(res.body == content);
    }
    ASSERT_EQ(404, httpp::client::fetch(srv.url("/missing")).status);
    fs::remove(path);
}

UTEST(httpp_server_response, a_body_can_be_streamed_in_pieces_in_sync_and_async_routes) {
    auto make_stream = [] {
        auto n = std::make_shared<int>(0);
        return [n](std::size_t, std::string& chunk) {
            chunk = "piece" + std::to_string(*n) + ";";
            return ++*n < 5;
        };
    };
    local_server srv([&](httpp::server& s) {
        s.get("/s", [&](const httpp::request&, httpp::response& res) { res.stream = make_stream(); });
        s.get_async("/as", [&](const httpp::request&, httpp::responder r) {
            httpp::response res;
            res.stream = make_stream();
            r.send(res);
        });
    });
    for (const char* route : {"/s", "/as"}) {
        auto res = httpp::client::fetch(srv.url(route));
        ASSERT_EQ(200, res.status);
        ASSERT_STREQ("piece0;piece1;piece2;piece3;piece4;", res.body.c_str());
    }
}

UTEST(httpp_server_settings, thread_pool_size_caps_how_many_sync_handlers_run_at_once) {
    std::atomic<int> now{0}, peak{0};
    local_server srv([&](httpp::server& s) {
        s.set_thread_pool(2, 2);
        s.get("/w", [&](const httpp::request&, httpp::response& res) {
            int n = ++now;
            int p = peak.load();
            while (n > p && !peak.compare_exchange_weak(p, n)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            --now;
            res.body = "ok";
        });
    });
    std::vector<std::future<httpp::response>> futures;
    for (int i = 0; i < 6; i++) futures.push_back(httpp::client::request(srv.url("/w")).run_async());
    for (auto& f : futures) ASSERT_EQ(200, f.get().status);
    ASSERT_LE(peak.load(), 2);
}

UTEST(httpp_server_settings, max_body_rejects_larger_requests) {
    local_server srv([](httpp::server& s) {
        s.set_max_body(1000);
        s.post("/up", [](const httpp::request& req, httpp::response& res) { res.body = std::to_string(req.body.size()); });
    });
    auto small = httpp::client::request(srv.url("/up")).data(std::string(500, 'a')).run();
    ASSERT_EQ(200, small.status);
    auto big = httpp::client::request(srv.url("/up")).data(std::string(50000, 'a')).timeout(5).run();
    ASSERT_TRUE(big.status == 413 || big.failed()); // refused either way, never served
}
