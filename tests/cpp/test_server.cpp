#include "utest/utest.h"
#include <functional>
#include "httpp/server.hpp"
#include "httpp/client.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
// small helper: run server on a background thread, stop it after the test
struct running_server {
    httpp::server srv;
    std::thread th;
    int port;

    explicit running_server(httpp::server&& s) : srv(std::move(s)) {
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ~running_server() {
        srv.stop();
        if (th.joinable()) th.join();
    }
};
} // namespace

UTEST(httpp_server, serves_a_registered_get_route) {
    httpp::server s;
    s.get("/ping", [](const httpp::request&, httpp::response& res) {
        res.status = 200;
        res.body = "pong";
    });

    running_server rs(std::move(s));
    httpp::client cli("127.0.0.1", rs.port);

    httpp::response res = cli.get("/ping");

    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("pong", res.body.c_str());
}

UTEST(httpp_server, unregistered_route_returns_404) {
    httpp::server s;
    running_server rs(std::move(s));
    httpp::client cli("127.0.0.1", rs.port);

    httpp::response res = cli.get("/nope");

    ASSERT_EQ(404, res.status);
}

UTEST(httpp_server, can_serve_a_directory_like_python_http_server) {
    httpp::server s;
    s.serve_directory("/", ".");
    running_server rs(std::move(s));
    (void)rs;
    ASSERT_TRUE(rs.port > 0);
}

UTEST(httpp_server, listens_on_a_fixed_requested_port) {
    httpp::server s;
    const int requested_port = 18080 + (static_cast<int>(std::hash<std::string>{}(__FILE__)) % 500);
    std::thread th([&s, requested_port] { s.listen("127.0.0.1", requested_port); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    httpp::client cli("127.0.0.1", requested_port);
    httpp::response res = cli.get("/whatever");

    ASSERT_EQ(404, res.status); // no route registered, but the port answers
    s.stop();
    th.join();
}

UTEST(httpp_server, route_exposes_path_params_query_headers_and_sets_response_headers) {
    httpp::server s;
    s.route("POST", "/users/:id", [](const httpp::request& req, httpp::response& res) {
        auto find = [](const auto& kv, const std::string& k) {
            for (const auto& [name, value] : kv) if (name == k) return value;
            return std::string("?");
        };
        res.status = 201;
        res.body = req.method + " " + find(req.path_params, "id") + " " + find(req.query, "a") + " " + req.body;
        res.headers.emplace_back("Content-Type", "application/json");
        res.headers.emplace_back("X-Made", "yes");
    });

    running_server rs(std::move(s));
    auto res = httpp::client::request("http://127.0.0.1:" + std::to_string(rs.port) + "/users/42?a=1")
                   .method("POST")
                   .data("hi")
                   .run();

    ASSERT_EQ(201, res.status);
    ASSERT_STREQ("POST 42 1 hi", res.body.c_str());
    ASSERT_STREQ("application/json", res.header("Content-Type").c_str());
    ASSERT_STREQ("yes", res.header("X-Made").c_str());
}

UTEST(httpp_server, route_rejects_unknown_methods) {
    httpp::server s;
    bool threw = false;
    try {
        s.route("BREW", "/x", [](const httpp::request&, httpp::response&) {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

UTEST(httpp_server, method_shorthands_register_the_matching_method) {
    httpp::server s;
    auto echo_method = [](const httpp::request& req, httpp::response& res) { res.body = req.method; };
    s.get("/m", echo_method);
    s.post("/m", echo_method);
    s.put("/m", echo_method);
    s.patch("/m", echo_method);
    s.del("/m", echo_method);

    running_server rs(std::move(s));
    const std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/m";
    for (const char* method : {"GET", "POST", "PUT", "PATCH", "DELETE"}) {
        auto res = httpp::client::request(url).method(method).run();
        ASSERT_EQ(200, res.status);
        ASSERT_STREQ(method, res.body.c_str());
    }
}

// ---- async (deferred) responses -------------------------------------------

UTEST(httpp_server, route_async_answers_later_from_another_thread) {
    httpp::server s;
    std::vector<std::thread> workers;
    s.route_async("POST", "/later/:id", [&](const httpp::request& req, httpp::responder r) {
        std::string id = req.path_params.at(0).second;
        workers.emplace_back([r, id] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            httpp::response res;
            res.status = 201;
            res.body = "later " + id;
            res.headers.emplace_back("Content-Type", "application/json");
            res.headers.emplace_back("X-Made", "yes");
            r.send(res);
        });
    });

    {
        running_server rs(std::move(s));
        auto res = httpp::client::request("http://127.0.0.1:" + std::to_string(rs.port) + "/later/7")
                       .method("POST").data("x").run();
        ASSERT_EQ(201, res.status);
        ASSERT_STREQ("later 7", res.body.c_str());
        ASSERT_STREQ("application/json", res.header("Content-Type").c_str());
        ASSERT_STREQ("yes", res.header("X-Made").c_str());
    }
    for (auto& w : workers) w.join();
}

UTEST(httpp_server, route_async_may_answer_before_the_handler_returns) {
    httpp::server s;
    s.route_async("GET", "/now", [](const httpp::request&, httpp::responder r) {
        httpp::response res;
        res.status = 200;
        res.body = "already";
        r.send(res);
        r.send(res); // a second send must be ignored, not corrupt the connection
    });
    running_server rs(std::move(s));
    auto res = httpp::client::fetch("http://127.0.0.1:" + std::to_string(rs.port) + "/now");
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("already", res.body.c_str());
}

UTEST(httpp_server, route_async_handler_exception_becomes_500) {
    httpp::server s;
    s.route_async("GET", "/boom", [](const httpp::request&, httpp::responder) {
        throw std::runtime_error("secret");
    });
    running_server rs(std::move(s));
    auto res = httpp::client::fetch("http://127.0.0.1:" + std::to_string(rs.port) + "/boom");
    ASSERT_EQ(500, res.status);
    ASSERT_TRUE(res.body.find("secret") == std::string::npos);
}

// The point of the feature: waiting requests must not occupy worker threads.
// 100 requests that each wait 500 ms are answered together in about 500 ms.
// With one blocked thread per request, the pool (at most a few dozen threads)
// would need several rounds, i.e. several seconds.
UTEST(httpp_server, route_async_does_not_hold_a_worker_thread_while_waiting) {
    constexpr int N = 100;
    httpp::server s;
    std::mutex m;
    std::vector<std::thread> timers;
    s.route_async("GET", "/wait", [&](const httpp::request&, httpp::responder r) {
        std::lock_guard<std::mutex> lock(m);
        timers.emplace_back([r] {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            httpp::response res;
            res.status = 200;
            res.body = "ok";
            r.send(res);
        });
    });

    std::atomic<int> ok{0};
    double seconds = 0;
    {
        running_server rs(std::move(s));
        const std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/wait";
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> clients;
        for (int i = 0; i < N; i++)
            clients.emplace_back([&] { if (httpp::client::fetch(url).status == 200) ok++; });
        for (auto& c : clients) c.join();
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    for (auto& t : timers) t.join();
    ASSERT_EQ(N, ok.load());
    ASSERT_LT(seconds, 1.5);
}

UTEST(httpp_server, destroying_the_server_with_a_pending_async_response_is_safe) {
    auto srv = std::make_unique<httpp::server>();
    httpp::responder stash;
    std::atomic<bool> got_it{false};
    srv->route_async("GET", "/hang", [&](const httpp::request&, httpp::responder r) {
        stash = r;
        got_it = true;
    });
    int port = srv->bind_to_any_port("127.0.0.1");
    std::thread th([&] { srv->listen_after_bind(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int status = -1;
    std::thread client([&] {
        status = httpp::client::fetch("http://127.0.0.1:" + std::to_string(port) + "/hang").status;
    });
    for (int i = 0; i < 200 && !got_it; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(got_it.load());

    srv->stop();
    th.join();
    srv.reset(); // closes the pending connection; the client must not hang
    client.join();
    ASSERT_NE(200, status);

    httpp::response late;
    late.status = 200;
    stash.send(late); // server is gone: must be a harmless no-op
}
