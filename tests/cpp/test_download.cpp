#include "utest/utest.h"
#include "httpp/download.hpp"
#include "httpp/server.hpp"

#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <future>

namespace {
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

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

UTEST(httpp_download, downloads_body_to_a_file) {
    httpp::server s;
    s.get("/data", [](const httpp::request&, httpp::response& res) {
        res.status = 200;
        res.body = "the quick brown fox";
    });
    running_server rs(std::move(s));

    const std::string dest = "test_download_output.txt";
    std::remove(dest.c_str());

    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/data";
    httpp::download_result result = httpp::download(url, dest, /*show_progress=*/false);

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(200, result.status);
    ASSERT_STREQ("the quick brown fox", read_file(dest).c_str());

    std::remove(dest.c_str());
}

UTEST(httpp_download, reports_failure_for_404) {
    httpp::server s;
    running_server rs(std::move(s));

    const std::string dest = "test_download_missing.txt";
    std::remove(dest.c_str());

    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/nope";
    httpp::download_result result = httpp::download(url, dest, false);

    ASSERT_FALSE(result.ok);
    ASSERT_EQ(404, result.status);

    std::remove(dest.c_str());
}

UTEST(httpp_download, builder_form_works_with_chained_calls) {
    httpp::server s;
    s.get("/f", [](const httpp::request&, httpp::response& res) {
        res.status = 200;
        res.body = "builder";
    });
    running_server rs(std::move(s));

    const std::string dest = "test_download_builder.txt";
    std::remove(dest.c_str());

    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/f";
    httpp::download_result result = httpp::download_file(url)
                                         .output(dest)
                                         .disable_progress()
                                         .run();

    ASSERT_TRUE(result.ok);
    ASSERT_STREQ("builder", read_file(dest).c_str());

    std::remove(dest.c_str());
}

UTEST(httpp_download, run_async_returns_a_future) {
    httpp::server s;
    s.get("/f", [](const httpp::request&, httpp::response& res) {
        res.status = 200;
        res.body = "async";
    });
    running_server rs(std::move(s));

    const std::string dest = "test_download_async.txt";
    std::remove(dest.c_str());

    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/f";
    std::future<httpp::download_result> fut = httpp::download_file(url)
                                                   .output(dest)
                                                   .disable_progress()
                                                   .run_async();

    httpp::download_result result = fut.get();

    ASSERT_TRUE(result.ok);
    ASSERT_STREQ("async", read_file(dest).c_str());

    std::remove(dest.c_str());
}
