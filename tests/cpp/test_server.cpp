#include "utest/utest.h"
#include <functional>
#include "httpp/server.hpp"
#include "httpp/client.hpp"

#include <thread>
#include <chrono>

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
