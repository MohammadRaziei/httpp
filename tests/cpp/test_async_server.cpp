// The HTTP server engine (src/core/async_server.cpp), driven with a raw Asio client so every byte
// on the wire is visible: framing, keep-alive, pipelining, limits, timeouts, streaming, shutdown.
#include "utest/utest.h"
#include "internal/async_server.hpp"
#include "internal/worker_pool.hpp"

#ifndef ASIO_STANDALONE
#  define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

using namespace httpp::detail;
using asio::ip::tcp;
using namespace std::chrono_literals;

namespace {

using handler_fn = std::function<wire_response(const http_request&)>;

// Engine + pool + loop thread. The handler runs on the pool, like a real route would.
struct fixture {
    worker_pool pool{4, 8};
    std::unique_ptr<async_server> srv;
    std::thread th;
    int port = 0;
    std::atomic<int> requests{0};

    fixture(handler_fn h, async_server::options opt = {}, bool answer_inline_from_thread = false) {
        (void)answer_inline_from_thread;
        srv = std::make_unique<async_server>(
            opt,
            [this, h](http_request r, async_server::answer_fn answer) {
                ++requests;
                return pool.enqueue([h, r = std::move(r), answer = std::move(answer)] { answer(h(r)); });
            },
            [this](std::function<void()> job) { return pool.enqueue(std::move(job)); });
        port = srv->bind("127.0.0.1", 0);
        th = std::thread([this] { srv->run(); });
    }
    ~fixture() {
        srv->stop();
        if (th.joinable()) th.join();
        pool.shutdown();
    }
};

wire_response text(int status, const std::string& body) {
    wire_response r;
    r.status = status;
    r.headers.emplace_back("Content-Type", "text/plain");
    r.body = body;
    return r;
}

// A raw client. read_for() collects bytes until `done(buffer)` is true, EOF, or the time is up.
struct raw_client {
    asio::io_context io;
    tcp::socket sock{io};
    std::string buf;
    bool eof = false;

    explicit raw_client(int port) {
        sock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)));
    }
    void send(const std::string& s) { asio::write(sock, asio::buffer(s)); }
    // Returns true if `done` was satisfied (false on timeout or EOF first).
    bool read_until(const std::function<bool(const std::string&)>& done, int ms = 5000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        std::string chunk(8192, '\0');
        while (!done(buf)) {
            if (eof) return false;
            bool got = false;
            std::error_code result;
            io.restart();
            sock.async_read_some(asio::buffer(chunk), [&](std::error_code ec, std::size_t n) {
                got = true;
                result = ec;
                if (!ec) buf.append(chunk.data(), n);
            });
            io.run_until(end);
            if (!got) { sock.cancel(); io.restart(); io.poll(); return false; }
            if (result) eof = true;
        }
        return true;
    }
    bool wait_eof(int ms = 5000) {
        read_until([](const std::string&) { return false; }, ms);
        return eof;
    }
    bool has(const std::string& s, int ms = 5000) {
        return read_until([&](const std::string& b) { return b.find(s) != std::string::npos; }, ms);
    }
};

std::size_t count(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
    return n;
}

} // namespace

UTEST(async_server, answers_a_request_and_adds_framing_headers) {
    fixture f([](const http_request&) { return text(200, "hello"); });
    raw_client c(f.port);
    c.send("GET /x HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_TRUE(c.has("hello"));
    ASSERT_TRUE(c.buf.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    ASSERT_TRUE(c.buf.find("Content-Length: 5\r\n") != std::string::npos);
    ASSERT_TRUE(c.buf.find("Date: ") != std::string::npos);
    ASSERT_TRUE(c.buf.find("Content-Type: text/plain\r\n") != std::string::npos);
    ASSERT_TRUE(c.buf.find("Connection: close") == std::string::npos); // HTTP/1.1 keeps alive
}

UTEST(async_server, passes_method_target_headers_and_body_to_the_handler) {
    std::mutex mu;
    http_request seen;
    fixture f([&](const http_request& r) { std::lock_guard<std::mutex> lk(mu); seen = r; return text(201, "ok"); });
    raw_client c(f.port);
    c.send("POST /a/b?x=1&y=%20 HTTP/1.1\r\nHost: t\r\nX-One: 1\r\nX-One: 2\r\nContent-Length: 5\r\n\r\nabcde");
    ASSERT_TRUE(c.has("ok"));
    std::lock_guard<std::mutex> lk(mu);
    ASSERT_STREQ("POST", seen.method.c_str());
    ASSERT_STREQ("/a/b?x=1&y=%20", seen.target.c_str());
    ASSERT_STREQ("abcde", seen.body.c_str());
    int ones = 0;
    for (auto& h : seen.headers) if (h.first == "X-One") ++ones;
    ASSERT_EQ(2, ones); // repeated headers survive in wire order
}

UTEST(async_server, decodes_a_chunked_request_body) {
    std::mutex mu;
    std::string body;
    fixture f([&](const http_request& r) { std::lock_guard<std::mutex> lk(mu); body = r.body; return text(200, "ok"); });
    raw_client c(f.port);
    c.send("POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n4\r\ndefg\r\n0\r\n\r\n");
    ASSERT_TRUE(c.has("ok"));
    std::lock_guard<std::mutex> lk(mu);
    ASSERT_STREQ("abcdefg", body.c_str());
}

UTEST(async_server, keep_alive_serves_several_requests_on_one_connection) {
    fixture f([](const http_request& r) { return text(200, r.target); });
    raw_client c(f.port);
    for (int i = 0; i < 3; ++i) {
        c.send("GET /" + std::to_string(i) + " HTTP/1.1\r\nHost: t\r\n\r\n");
        ASSERT_TRUE(c.has("\r\n\r\n/" + std::to_string(i)));
    }
    ASSERT_EQ(3u, count(c.buf, "HTTP/1.1 200"));
}

UTEST(async_server, pipelined_requests_are_answered_in_order) {
    fixture f([](const http_request& r) { return text(200, r.target); });
    raw_client c(f.port);
    c.send("GET /one HTTP/1.1\r\nHost: t\r\n\r\nGET /two HTTP/1.1\r\nHost: t\r\n\r\nPOST /three HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\nhi");
    ASSERT_TRUE(c.has("/three"));
    auto a = c.buf.find("/one"), b = c.buf.find("/two"), d = c.buf.find("/three");
    ASSERT_TRUE(a != std::string::npos && b != std::string::npos);
    ASSERT_TRUE(a < b && b < d);
    ASSERT_EQ(3, f.requests.load());
}

UTEST(async_server, a_request_arriving_a_byte_at_a_time_is_served) {
    fixture f([](const http_request& r) { return text(200, r.body); });
    raw_client c(f.port);
    const std::string req = "POST /s HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nping";
    for (char ch : req) { c.send(std::string(1, ch)); std::this_thread::sleep_for(2ms); }
    ASSERT_TRUE(c.has("ping"));
}

UTEST(async_server, connection_close_and_http_1_0_end_the_connection) {
    fixture f([](const http_request&) { return text(200, "bye"); });
    {
        raw_client c(f.port);
        c.send("GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
        ASSERT_TRUE(c.has("bye"));
        ASSERT_TRUE(c.buf.find("Connection: close") != std::string::npos);
        ASSERT_TRUE(c.wait_eof());
    }
    {
        raw_client c(f.port);
        c.send("GET / HTTP/1.0\r\n\r\n");
        ASSERT_TRUE(c.has("bye"));
        ASSERT_TRUE(c.wait_eof());
    }
    {
        raw_client c(f.port); // HTTP/1.0 may ask to keep the connection
        c.send("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
        ASSERT_TRUE(c.has("bye"));
        ASSERT_TRUE(c.buf.find("Connection: keep-alive") != std::string::npos);
        c.send("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
        ASSERT_TRUE(c.read_until([](const std::string& b) { return count(b, "bye") == 2; }));
    }
}

UTEST(async_server, keep_alive_max_limits_requests_per_connection) {
    async_server::options o;
    o.keep_alive_max = 2;
    fixture f([](const http_request&) { return text(200, "x"); }, o);
    raw_client c(f.port);
    c.send("GET /1 HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_TRUE(c.read_until([](const std::string& b) { return count(b, "HTTP/1.1 200") == 1 && b.find("\r\n\r\nx") != std::string::npos; }));
    ASSERT_TRUE(c.buf.find("Connection: close") == std::string::npos);
    c.send("GET /2 HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_TRUE(c.read_until([](const std::string& b) { return count(b, "HTTP/1.1 200") == 2; }));
    ASSERT_TRUE(c.buf.find("Connection: close") != std::string::npos); // told on the last one
    ASSERT_TRUE(c.wait_eof());
}

UTEST(async_server, idle_connection_is_closed_after_the_keep_alive_timeout) {
    async_server::options o;
    o.keep_alive_timeout_sec = 1;
    fixture f([](const http_request&) { return text(200, "x"); }, o);
    raw_client c(f.port);
    c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_TRUE(c.has("x"));
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(c.wait_eof(5000));
    ASSERT_TRUE(std::chrono::steady_clock::now() - t0 < 4s);
    ASSERT_EQ(1, f.requests.load());
}

UTEST(async_server, a_stalled_request_hits_the_read_timeout) {
    async_server::options o;
    o.read_timeout_sec = 1;
    o.keep_alive_timeout_sec = 30;
    fixture f([](const http_request&) { return text(200, "x"); }, o);
    raw_client c(f.port);
    c.send("POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 10\r\n\r\nabc"); // 7 bytes never come
    ASSERT_TRUE(c.wait_eof(5000));
    ASSERT_EQ(0, f.requests.load());
}

UTEST(async_server, idle_connections_cost_no_worker_threads) {
    std::atomic<int> running{0};
    fixture f([&](const http_request&) { ++running; return text(200, "x"); });
    std::vector<std::unique_ptr<raw_client>> idle;
    for (int i = 0; i < 200; ++i) idle.push_back(std::make_unique<raw_client>(f.port)); // all silent
    raw_client c(f.port);
    c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_TRUE(c.has("x")); // answered although 200 connections sit idle (pool has 4 threads)
    ASSERT_EQ(1, running.load());
}

UTEST(async_server, oversized_body_gets_413_and_the_client_can_still_read_it) {
    async_server::options o;
    o.max_body = 1000;
    fixture f([](const http_request&) { return text(200, "never"); }, o);
    raw_client c(f.port);
    c.send("POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 50000\r\n\r\n" + std::string(30000, 'a'));
    ASSERT_TRUE(c.has("413"));
    ASSERT_TRUE(c.buf.find("Connection: close") != std::string::npos);
    ASSERT_EQ(0, f.requests.load());
}

UTEST(async_server, oversized_chunked_body_gets_413) {
    async_server::options o;
    o.max_body = 100;
    fixture f([](const http_request&) { return text(200, "never"); }, o);
    raw_client c(f.port);
    c.send("POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n" + std::string("200\r\n") + std::string(512, 'a') + "\r\n0\r\n\r\n");
    ASSERT_TRUE(c.has("413"));
    ASSERT_EQ(0, f.requests.load());
}

UTEST(async_server, long_uri_and_huge_header_are_refused) {
    fixture f([](const http_request&) { return text(200, "never"); });
    {
        raw_client c(f.port);
        c.send("GET /" + std::string(9000, 'a') + " HTTP/1.1\r\nHost: t\r\n\r\n");
        ASSERT_TRUE(c.has("414"));
    }
    {
        raw_client c(f.port);
        c.send("GET / HTTP/1.1\r\nHost: t\r\nX-Big: " + std::string(9000, 'b') + "\r\n\r\n");
        ASSERT_TRUE(c.has("431"));
    }
    ASSERT_EQ(0, f.requests.load());
}

UTEST(async_server, garbage_gets_400_and_the_connection_closes) {
    fixture f([](const http_request&) { return text(200, "never"); });
    raw_client c(f.port);
    c.send("this is not http\r\n\r\n");
    ASSERT_TRUE(c.has("400"));
    ASSERT_TRUE(c.wait_eof());
    ASSERT_EQ(0, f.requests.load());
}

UTEST(async_server, expect_100_continue_gets_the_interim_response) {
    fixture f([](const http_request& r) { return text(200, r.body); });
    raw_client c(f.port);
    c.send("POST / HTTP/1.1\r\nHost: t\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n");
    ASSERT_TRUE(c.has("100 Continue"));
    c.send("xyz");
    ASSERT_TRUE(c.has("xyz"));
}

UTEST(async_server, head_gets_headers_and_the_length_but_no_body) {
    fixture f([](const http_request&) { return text(200, "0123456789"); });
    raw_client c(f.port);
    c.send("HEAD / HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_TRUE(c.has("\r\n\r\n"));
    ASSERT_TRUE(c.buf.find("Content-Length: 10") != std::string::npos);
    c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n"); // the connection is still in sync
    ASSERT_TRUE(c.has("0123456789"));
    ASSERT_EQ(1u, count(c.buf, "0123456789"));
}

UTEST(async_server, no_body_statuses_have_no_content_length) {
    fixture f([](const http_request&) { wire_response r; r.status = 204; return r; });
    raw_client c(f.port);
    c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_TRUE(c.has("\r\n\r\n"));
    ASSERT_TRUE(c.buf.find("Content-Length") == std::string::npos);
    ASSERT_TRUE(c.buf.find("204 No Content") != std::string::npos);
}

UTEST(async_server, handler_headers_cannot_override_the_framing) {
    fixture f([](const http_request&) {
        auto r = text(200, "abc");
        r.headers.emplace_back("Content-Length", "999");
        r.headers.emplace_back("Connection", "upgrade");
        r.headers.emplace_back("X-Mine", "1");
        return r;
    });
    raw_client c(f.port);
    c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_TRUE(c.has("abc"));
    ASSERT_TRUE(c.buf.find("Content-Length: 3") != std::string::npos);
    ASSERT_TRUE(c.buf.find("999") == std::string::npos);
    ASSERT_TRUE(c.buf.find("upgrade") == std::string::npos);
    ASSERT_TRUE(c.buf.find("X-Mine: 1") != std::string::npos);
}

UTEST(async_server, streamed_body_is_chunked_when_the_length_is_unknown) {
    fixture f([](const http_request&) {
        wire_response r;
        r.status = 200;
        auto n = std::make_shared<int>(0);
        r.next = [n](std::string& piece) { piece = "part" + std::to_string(*n); return ++*n < 3; };
        return r;
    });
    raw_client c(f.port);
    c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_TRUE(c.has("0\r\n\r\n"));
    ASSERT_TRUE(c.buf.find("Transfer-Encoding: chunked") != std::string::npos);
    ASSERT_TRUE(c.buf.find("5\r\npart0\r\n5\r\npart1\r\n5\r\npart2\r\n0\r\n\r\n") != std::string::npos);
}

UTEST(async_server, streamed_body_with_a_known_length_is_not_chunked) {
    fixture f([](const http_request&) {
        wire_response r;
        r.status = 200;
        r.length = 6;
        auto n = std::make_shared<int>(0);
        r.next = [n](std::string& piece) { piece = "abc"; return ++*n < 2; };
        return r;
    });
    raw_client c(f.port);
    c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_TRUE(c.has("abcabc"));
    ASSERT_TRUE(c.buf.find("Content-Length: 6") != std::string::npos);
    ASSERT_TRUE(c.buf.find("chunked") == std::string::npos);
    c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n"); // connection still usable
    ASSERT_TRUE(c.read_until([](const std::string& b) { return count(b, "abcabc") == 2; }));
}

UTEST(async_server, a_stream_that_throws_midway_drops_the_connection_without_an_ending) {
    fixture f([](const http_request&) {
        wire_response r;
        r.status = 200;
        auto n = std::make_shared<int>(0);
        r.next = [n](std::string& piece) -> bool {
            if (*n == 1) throw std::runtime_error("boom");
            piece = "first";
            ++*n;
            return true;
        };
        return r;
    });
    raw_client c(f.port);
    c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_TRUE(c.wait_eof());
    ASSERT_TRUE(c.buf.find("first") != std::string::npos);
    ASSERT_TRUE(c.buf.find("0\r\n\r\n") == std::string::npos); // never ended cleanly
}

UTEST(async_server, a_known_length_stream_that_comes_up_short_drops_the_connection) {
    fixture f([](const http_request&) {
        wire_response r;
        r.status = 200;
        r.length = 100;
        r.next = [](std::string& piece) { piece = "tiny"; return false; };
        return r;
    });
    raw_client c(f.port);
    c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_TRUE(c.wait_eof());
}

UTEST(async_server, a_generator_is_released_when_its_response_ends) {
    auto token = std::make_shared<int>(1);
    std::weak_ptr<int> weak = token;
    {
        fixture f([token](const http_request&) {
            wire_response r;
            r.status = 200;
            r.next = [token](std::string& piece) { piece = "x"; return false; };
            return r;
        });
        token.reset();
        raw_client c(f.port);
        c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
        ASSERT_TRUE(c.has("0\r\n\r\n"));
        c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
        ASSERT_TRUE(c.read_until([](const std::string& b) { return count(b, "0\r\n\r\n") == 2; }));
    }
    ASSERT_TRUE(weak.expired());
}

UTEST(async_server, an_answer_can_come_later_from_another_thread) {
    std::mutex mu;
    std::vector<async_server::answer_fn> parked;
    async_server::options o;
    auto pool = std::make_shared<worker_pool>(1, 1);
    async_server srv(o,
        [&](http_request, async_server::answer_fn a) { std::lock_guard<std::mutex> lk(mu); parked.push_back(std::move(a)); return true; },
        [&](std::function<void()> j) { return pool->enqueue(std::move(j)); });
    const int port = srv.bind("127.0.0.1", 0);
    std::thread loop([&] { srv.run(); });
    raw_client c(port);
    c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    std::this_thread::sleep_for(100ms);
    ASSERT_TRUE(c.buf.empty()); // nothing yet: the handler "returned" without answering
    std::thread late([&] {
        std::lock_guard<std::mutex> lk(mu);
        parked.at(0)(text(200, "late"));
        parked.at(0)(text(500, "second answer is ignored"));
    });
    ASSERT_TRUE(c.has("late"));
    late.join();
    srv.stop();
    loop.join();
    ASSERT_TRUE(c.buf.find("second answer") == std::string::npos);
}

UTEST(async_server, answering_after_the_server_is_gone_is_harmless) {
    async_server::answer_fn saved;
    {
        worker_pool pool(1, 1);
        async_server srv({},
            [&](http_request, async_server::answer_fn a) { saved = std::move(a); return true; },
            [&](std::function<void()> j) { return pool.enqueue(std::move(j)); });
        const int port = srv.bind("127.0.0.1", 0);
        std::thread loop([&] { srv.run(); });
        raw_client c(port);
        c.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
        std::this_thread::sleep_for(100ms);
        srv.stop();
        loop.join();
    }
    saved(text(200, "too late")); // the engine is destroyed: must not crash
    ASSERT_TRUE(true);
}

UTEST(async_server, stop_closes_every_connection_and_run_returns) {
    auto f = std::make_unique<fixture>([](const http_request&) { return text(200, "x"); });
    raw_client a(f->port), b(f->port);
    a.send("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
    ASSERT_TRUE(a.has("x"));
    f.reset(); // stop() + join
    ASSERT_TRUE(a.wait_eof());
    ASSERT_TRUE(b.wait_eof());
}

UTEST(async_server, a_second_server_cannot_bind_a_port_that_is_being_served) {
    fixture f([](const http_request&) { return text(200, "x"); });
    async_server other({}, [](http_request, async_server::answer_fn) { return true; },
                       [](std::function<void()>) { return false; });
    ASSERT_EQ(-1, other.bind("127.0.0.1", f.port));
}

UTEST(async_server, stop_requested_before_run_is_honoured) {
    worker_pool pool(1, 1);
    async_server srv({}, [](http_request, async_server::answer_fn) { return true; },
                     [&](std::function<void()> j) { return pool.enqueue(std::move(j)); });
    ASSERT_TRUE(srv.bind("127.0.0.1", 0) > 0);
    srv.stop();
    auto done = std::async(std::launch::async, [&] { return srv.run(); });
    ASSERT_TRUE(done.wait_for(5s) == std::future_status::ready);
    ASSERT_TRUE(done.get());
}

UTEST(async_server, many_clients_at_once) {
    fixture f([](const http_request& r) { return text(200, r.target); });
    std::atomic<int> good{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 40; ++i) ts.emplace_back([&, i] {
        raw_client c(f.port);
        for (int k = 0; k < 5; ++k) {
            const std::string t = "/" + std::to_string(i) + "_" + std::to_string(k);
            c.send("GET " + t + " HTTP/1.1\r\nHost: t\r\n\r\n");
            if (!c.has("\r\n\r\n" + t)) return;
        }
        ++good;
    });
    for (auto& t : ts) t.join();
    ASSERT_EQ(40, good.load());
}
