#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"

#include <thread>
#include <chrono>

// This test never touches httplib.h — httpp::server (already compiled into
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
        srv.get("/hello", [](const httpp::request&, httpp::response& res) {
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

UTEST(httpp_client, fetch_parses_a_full_url_and_gets_it) {
    TestServer server;
    std::string full_url = "http://127.0.0.1:" + std::to_string(server.port) + "/hello";

    httpp::response res = httpp::client::fetch(full_url);

    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("world", res.body.c_str());
}
