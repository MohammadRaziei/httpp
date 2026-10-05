#pragma once
// A server on an ephemeral port, serving until destroyed. Shared by the C++ tests.
#include "httpp/server.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace testutil {

struct local_server {
    httpp::server srv;
    int port = 0;
    std::thread th;

    explicit local_server(std::function<void(httpp::server&)> setup) {
        setup(srv);
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ~local_server() {
        srv.stop();
        th.join();
    }
    std::string url(const std::string& path = "") const { return "http://127.0.0.1:" + std::to_string(port) + path; }
};

} // namespace testutil
