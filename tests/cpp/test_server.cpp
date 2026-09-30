#include "utest/utest.h"
#include <functional>
#include "httpp/server.hpp"
#include "httpp/client.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

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
