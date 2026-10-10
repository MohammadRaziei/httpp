// The non-blocking client engine (src/core/async_http.cpp): Asio sockets + llhttp.
#include "utest/utest.h"
#include "local_server.hpp"
#include "internal/async_http.hpp"

#ifndef ASIO_STANDALONE
#  define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

using httpp::detail::async_http;
using httpp::detail::async_job;

namespace {

std::future<httpp::response> run(async_http& eng, async_job job) {
    auto p = std::make_shared<std::promise<httpp::response>>();
    auto f = p->get_future();
    eng.submit(std::move(job), [p](httpp::response r) { p->set_value(std::move(r)); });
    return f;
}

async_job job_for(int port, std::string target = "/", std::string method = "GET") {
    async_job j;
    j.host = "127.0.0.1";
    j.port = port;
    j.target = std::move(target);
    j.method = std::move(method);
    return j;
}

bool ready(std::future<httpp::response>& f, int seconds = 10) {
    return f.wait_for(std::chrono::seconds(seconds)) == std::future_status::ready;
}

// A scripted one-connection server: reads the request head, then writes each piece with a pause.
struct raw_server {
    asio::io_context io;
    asio::ip::tcp::acceptor acc{io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
    asio::ip::tcp::socket s{io};
    std::thread th;
    std::string request;
    int port() { return acc.local_endpoint().port(); }

    raw_server(std::vector<std::string> pieces, bool close_after = true) {
        th = std::thread([this, pieces = std::move(pieces), close_after] {
            std::error_code ec;
            acc.accept(s, ec);
            if (ec) return;
            asio::streambuf buf;
            asio::read_until(s, buf, "\r\n\r\n", ec);
            request.assign(asio::buffers_begin(buf.data()), asio::buffers_end(buf.data()));
            for (const auto& p : pieces) {
                asio::write(s, asio::buffer(p), ec);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (close_after) { s.shutdown(asio::ip::tcp::socket::shutdown_both, ec); return; }
            char c;
            while (s.read_some(asio::buffer(&c, 1), ec) > 0) {} // hold the connection open, silent, until ~raw_server
        });
    }
    ~raw_server() {
        std::error_code ec;
        acc.close(ec);
        s.shutdown(asio::ip::tcp::socket::shutdown_both, ec); // wakes the thread if it is waiting
        if (th.joinable()) th.join();
    }
};

} // namespace

UTEST(async_http, gets_a_body) {
    testutil::local_server server([](httpp::server& s) {
        s.get("/hello", [](const httpp::request&, httpp::response& r) { r.status = 200; r.body = "world"; });
    });
    async_http eng;
    auto f = run(eng, job_for(server.port, "/hello"));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("world", r.body.c_str());
    ASSERT_FALSE(r.failed());
}

UTEST(async_http, non_2xx_is_a_response_not_an_error) {
    testutil::local_server server([](httpp::server&) {});
    async_http eng;
    auto f = run(eng, job_for(server.port, "/missing"));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(404, r.status);
    ASSERT_FALSE(r.failed());
}

UTEST(async_http, posts_a_body_and_reads_headers) {
    testutil::local_server server([](httpp::server& s) {
        s.post("/echo", [](const httpp::request& q, httpp::response& r) {
            r.status = 201;
            r.body = q.body;
            r.headers.emplace_back("X-Seen", q.header("X-Token"));
        });
    });
    async_http eng;
    auto j = job_for(server.port, "/echo", "POST");
    j.body = std::string("a\0b", 3) + "payload";
    j.headers = {{"X-Token", "abc"}, {"Content-Type", "application/octet-stream"}};
    auto f = run(eng, std::move(j));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(201, r.status);
    ASSERT_TRUE(r.body == std::string("a\0b", 3) + "payload");
    ASSERT_STREQ("abc", r.header("x-seen").c_str());
}

UTEST(async_http, decodes_a_chunked_response) {
    testutil::local_server server([](httpp::server& s) {
        s.get("/stream", [](const httpp::request&, httpp::response& r) {
            r.status = 200;
            r.stream = [](std::size_t off, std::string& chunk) {
                if (off >= 3000) return false;
                chunk.assign(1000, static_cast<char>('a' + off / 1000));
                return true;
            };
        });
    });
    async_http eng;
    auto f = run(eng, job_for(server.port, "/stream"));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(r.body == std::string(1000, 'a') + std::string(1000, 'b') + std::string(1000, 'c'));
}

UTEST(async_http, head_has_no_body_but_keeps_headers) {
    testutil::local_server server([](httpp::server& s) {
        s.get("/h", [](const httpp::request&, httpp::response& r) { r.status = 200; r.body = "0123456789"; });
    });
    async_http eng;
    auto f = run(eng, job_for(server.port, "/h", "HEAD"));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(r.body.empty());
    ASSERT_STREQ("10", r.header("Content-Length").c_str());
}

UTEST(async_http, many_requests_in_flight_on_one_thread) {
    testutil::local_server server([](httpp::server& s) {
        s.get("/n", [](const httpp::request& q, httpp::response& r) { r.status = 200; r.body = q.query_param("i"); });
    });
    async_http eng;
    constexpr int N = 300;
    std::vector<std::future<httpp::response>> fs;
    for (int i = 0; i < N; ++i) fs.push_back(run(eng, job_for(server.port, "/n?i=" + std::to_string(i))));
    int good = 0;
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(ready(fs[i], 30));
        auto r = fs[i].get();
        if (r.status == 200 && r.body == std::to_string(i)) ++good;
    }
    ASSERT_EQ(N, good);
}

UTEST(async_http, connection_refused) {
    int port;
    { testutil::local_server server([](httpp::server&) {}); port = server.port; } // closed again
    async_http eng;
    auto f = run(eng, job_for(port));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(0, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::connection);
}

UTEST(async_http, unknown_host) {
    async_http eng;
    async_job j = job_for(80);
    j.host = "no-such-host.invalid";
    j.timeout_sec = 10;
    auto f = run(eng, std::move(j));
    ASSERT_TRUE(ready(f, 20));
    auto r = f.get();
    ASSERT_TRUE(r.error == httpp::error_kind::connection || r.error == httpp::error_kind::timeout);
}

UTEST(async_http, times_out_on_a_silent_server) {
    raw_server silent({}, /*close_after=*/false); // reads the request, never answers
    async_http eng;
    auto j = job_for(silent.port());
    j.timeout_sec = 1;
    auto t0 = std::chrono::steady_clock::now();
    auto f = run(eng, std::move(j));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    auto took = std::chrono::steady_clock::now() - t0;
    ASSERT_TRUE(r.error == httpp::error_kind::timeout);
    ASSERT_EQ(0, r.status);
    ASSERT_TRUE(took < std::chrono::seconds(5));
}

UTEST(async_http, max_response_size_gives_413_too_large) {
    testutil::local_server server([](httpp::server& s) {
        s.get("/big", [](const httpp::request&, httpp::response& r) { r.status = 200; r.body = std::string(50000, 'x'); });
    });
    async_http eng;
    auto j = job_for(server.port, "/big");
    j.max_response_size = 1000;
    auto f = run(eng, std::move(j));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(413, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::too_large);
}

UTEST(async_http, rejects_header_injection) {
    async_http eng;
    auto j = job_for(1);
    j.headers = {{"X-A", "ok\r\nInjected: 1"}};
    auto f = run(eng, std::move(j));
    ASSERT_TRUE(ready(f));
    auto e1 = f.get().error;
    ASSERT_TRUE(e1 == httpp::error_kind::other);

    auto j2 = job_for(1, "/x HTTP/1.1\r\nEvil: 1");
    auto f2 = run(eng, std::move(j2));
    ASSERT_TRUE(ready(f2));
    auto e2 = f2.get().error;
    ASSERT_TRUE(e2 == httpp::error_kind::other);
}

UTEST(async_http, response_delivered_a_byte_at_a_time) {
    std::string msg = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-Long-Header-Name: some value\r\n\r\nhello";
    std::vector<std::string> pieces;
    for (char c : msg) pieces.emplace_back(1, c);
    raw_server srv(pieces);
    async_http eng;
    auto f = run(eng, job_for(srv.port()));
    ASSERT_TRUE(ready(f, 20));
    auto r = f.get();
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("hello", r.body.c_str());
    ASSERT_STREQ("some value", r.header("X-Long-Header-Name").c_str());
}

UTEST(async_http, body_that_ends_at_connection_close) {
    raw_server srv({"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npart one, ", "part two"});
    async_http eng;
    auto f = run(eng, job_for(srv.port()));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("part one, part two", r.body.c_str());
}

UTEST(async_http, truncated_body_is_a_connection_error) {
    raw_server srv({"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly this much"});
    async_http eng;
    auto f = run(eng, job_for(srv.port()));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(0, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::connection);
}

UTEST(async_http, skips_an_informational_100_response) {
    raw_server srv({"HTTP/1.1 100 Continue\r\n\r\n", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"});
    async_http eng;
    auto f = run(eng, job_for(srv.port()));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("ok", r.body.c_str());
    ASSERT_TRUE(r.header("Content-Length") == "2");
}

UTEST(async_http, garbage_is_an_error_not_a_crash) {
    raw_server srv({"this is not http\r\n\r\n"});
    async_http eng;
    auto f = run(eng, job_for(srv.port()));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(0, r.status);
    ASSERT_TRUE(r.failed());
}

UTEST(async_http, destroying_the_engine_cancels_pending_requests) {
    raw_server silent({}, false);
    std::future<httpp::response> f;
    {
        async_http eng;
        f = run(eng, job_for(silent.port())); // no timeout, never answered
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(ready(f, 5));
    auto e = f.get().error;
    ASSERT_TRUE(e == httpp::error_kind::canceled);
}
