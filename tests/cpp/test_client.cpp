#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"

#include <thread>
#include <chrono>

// This test never touches httplib.h - httpp::server (already compiled into
// httpp_core, PIMPL-wrapped) is used to spin up a throwaway test server, the
// same way any other consumer of httpp would. That's the whole point of
// hiding httplib behind httpp::client/httpp::server: our own tests get to
// use just the public API too, with zero macro juggling.
namespace {

struct TestServer {
    httpp::server srv;
    std::thread th;
    int port = 0;

    TestServer() {
        srv.get("/hello", [](const httpp::server_request&, httpp::response& res) {
            res.status = 200;
            res.body = "world";
        });
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
        // give the listener a moment to actually accept()
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ~TestServer() {
        srv.stop();
        if (th.joinable()) th.join();
    }
};

} // namespace

UTEST(httpp_client, gets_body_from_local_server) {
    TestServer server;
    httpp::client cli("127.0.0.1", server.port);

    httpp::response res = cli.get("/hello");

    ASSERT_TRUE(res.ok());
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("world", res.body.c_str());
}

UTEST(httpp_client, returns_404_for_unknown_path) {
    TestServer server;
    httpp::client cli("127.0.0.1", server.port);

    httpp::response res = cli.get("/does-not-exist");

    ASSERT_FALSE(res.ok());
    ASSERT_EQ(404, res.status);
}

UTEST(httpp_client, fetch_keeps_the_trailing_slash_of_the_path) {
    // "/dir" and "/dir/" are two resources: the request line must say what the URL says (the parser used to drop the '/').
    httpp::server srv;
    srv.get("/dir", [](const httpp::server_request&, httpp::response& res) { res.body = "plain"; });
    srv.get("/dir/", [](const httpp::server_request&, httpp::response& res) { res.body = "slash"; });
    const int port = srv.bind_to_any_port("127.0.0.1");
    std::thread th([&srv] { srv.listen_after_bind(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    httpp::response with_slash = httpp::client::fetch(base + "/dir/");
    httpp::response without = httpp::client::fetch(base + "/dir");
    httpp::response dotted = httpp::client::fetch(base + "/x/../dir/");

    srv.stop();
    th.join();

    ASSERT_TRUE(with_slash.ok());
    ASSERT_STREQ("slash", with_slash.body.c_str());
    ASSERT_TRUE(without.ok());
    ASSERT_STREQ("plain", without.body.c_str());
    ASSERT_TRUE(dotted.ok());
    ASSERT_STREQ("slash", dotted.body.c_str());
}

UTEST(httpp_client, fetch_parses_a_full_url_and_gets_it) {
    TestServer server;
    std::string full_url = "http://127.0.0.1:" + std::to_string(server.port) + "/hello";

    httpp::response res = httpp::client::fetch(full_url);

    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("world", res.body.c_str());
}
