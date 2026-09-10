#include "utest/utest.h"
#include "httpp/client.hpp"

// httplib is used here ONLY to spin up a throwaway test server; it is not
// part of httpp's public surface. The thing under test is httpp::Client,
// whose public header (httpp/client.hpp) never includes httplib.h.
#include <httplib.h>

#include <thread>
#include <chrono>

namespace {

struct TestServer {
    httplib::Server svr;
    std::thread th;
    int port = 0;

    TestServer() {
        svr.Get("/hello", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("world", "text/plain");
        });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        // give the listener a moment to actually accept()
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ~TestServer() {
        svr.stop();
        if (th.joinable()) th.join();
    }
};

} // namespace

UTEST(httpp_client, gets_body_from_local_server) {
    TestServer server;
    httpp::Client client("127.0.0.1", server.port);

    httpp::Response res = client.get("/hello");

    ASSERT_TRUE(res.ok());
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("world", res.body.c_str());
}

UTEST(httpp_client, returns_404_for_unknown_path) {
    TestServer server;
    httpp::Client client("127.0.0.1", server.port);

    httpp::Response res = client.get("/does-not-exist");

    ASSERT_FALSE(res.ok());
    ASSERT_EQ(404, res.status);
}
