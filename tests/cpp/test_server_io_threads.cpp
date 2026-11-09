// Several I/O threads: each one runs its own acceptor on the same port (SO_REUSEPORT) and keeps its
// connections for good. Not available on Windows, where the server keeps one I/O thread.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32

using testutil::local_server;

namespace {

std::string thread_text() {
    std::ostringstream os;
    os << std::this_thread::get_id();
    return os.str();
}

void answer(httpp::responder& r, int status, const std::string& body) {
    httpp::response res;
    res.status = status;
    res.body = body;
    r.send(res);
}

} // namespace

// Every I/O thread watches the requests that wait on it (no list shared between threads): requests that land on any of them are timed out,
// and answered ones are left alone.
UTEST(httpp_server_io_threads, unanswered_io_requests_get_a_504_on_every_io_thread_and_answered_ones_are_not_touched) {
    std::mutex mu;
    std::vector<httpp::responder> kept;
    local_server srv([&](httpp::server& s) {
        s.set_io_threads(3);
        s.set_async_timeout(1);
        s.route_io("GET", "/hold", [&](std::shared_ptr<const httpp::request>, httpp::responder r) {
            std::lock_guard<std::mutex> lock(mu);
            kept.push_back(r);
        });
        s.route_io("GET", "/quick", [](std::shared_ptr<const httpp::request>, httpp::responder r) { answer(r, 200, "q"); });
    });
    constexpr int n = 12;  // enough connections that every I/O thread gets some
    std::vector<std::thread> clients;
    std::vector<int> status(n, 0);
    for (int i = 0; i < n; ++i)
        clients.emplace_back([&, i] { status[i] = httpp::client::request(srv.url("/hold")).timeout(10).run().status; });
    for (int i = 0; i < 30; ++i) ASSERT_EQ(200, httpp::client::request(srv.url("/quick")).timeout(10).run().status);
    for (auto& c : clients) c.join();
    for (int i = 0; i < n; ++i) ASSERT_EQ(504, status[i]);
    ASSERT_EQ(200, httpp::client::request(srv.url("/quick")).timeout(10).run().status);
}

UTEST(httpp_server_io_threads, connections_spread_over_the_io_threads_and_each_stays_on_its_thread) {
    local_server srv([](httpp::server& s) {
        s.set_io_threads(4);
        s.route_io("GET", "/who", [](std::shared_ptr<const httpp::request>, httpp::responder r) { answer(r, 200, thread_text()); });
    });
    std::mutex mu;
    std::set<std::string> seen;
    std::atomic<int> same_connection_moved{0};
    std::vector<std::thread> users;
    for (int i = 0; i < 40; ++i) {
        users.emplace_back([&] {
            // One real connection per user (httpp::client pools connections across sessions, so it cannot
            // pin one): five keep-alive requests on it must all be answered by the same I/O thread.
            asio::io_context io;
            asio::ip::tcp::socket sock(io);
            sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(srv.port)));
            asio::streambuf buf;
            std::string first;
            for (int k = 0; k < 5; ++k) {
                asio::write(sock, asio::buffer(std::string("GET /who HTTP/1.1\r\nHost: x\r\n\r\n")));
                asio::read_until(sock, buf, "\r\n\r\n");
                std::istream in(&buf);
                std::string line, head;
                std::size_t length = 0;
                while (std::getline(in, line) && line != "\r") {
                    std::string low;
                    for (char ch : line) low += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    if (low.rfind("content-length:", 0) == 0) length = static_cast<std::size_t>(std::stoul(low.substr(15)));
                }
                if (buf.size() < length) asio::read(sock, buf, asio::transfer_exactly(length - buf.size()));
                std::string id(length, '\0');
                in.read(&id[0], static_cast<std::streamsize>(length));
                if (k == 0) first = id;
                else if (id != first) ++same_connection_moved;
                std::lock_guard<std::mutex> lock(mu);
                seen.insert(id);
            }
        });
    }
    for (auto& t : users) t.join();
    ASSERT_EQ(0, same_connection_moved.load());
#ifdef __linux__
    ASSERT_TRUE(seen.size() >= 2); // 40 connections over 4 acceptors: all on one is (1/4)^39. Other systems (macOS) do not spread them.
#endif
    ASSERT_TRUE(seen.size() <= 4);
}

UTEST(httpp_server_io_threads, an_io_route_runs_on_an_io_thread_not_on_a_worker_and_the_pool_still_works) {
    local_server srv([](httpp::server& s) {
        s.set_io_threads(3);
        s.route_io("GET", "/io", [](std::shared_ptr<const httpp::request>, httpp::responder r) { answer(r, 200, "io"); });
        s.get("/pool", [](const httpp::request&, httpp::response& res) { res.body = "pool"; });
    });
    for (int i = 0; i < 20; ++i) {
        ASSERT_STREQ("io", httpp::client::fetch(srv.url("/io")).body.c_str());
        ASSERT_STREQ("pool", httpp::client::fetch(srv.url("/pool")).body.c_str());
    }
}

UTEST(httpp_server_io_threads, the_after_hook_and_an_unanswered_route_work_with_several_io_threads) {
    std::atomic<int> after_calls{0};
    local_server srv([&](httpp::server& s) {
        s.set_io_threads(3);
        s.set_async_timeout(1);
        s.after([&](const httpp::request&, httpp::response&) { ++after_calls; });
        s.route_io("GET", "/ok", [](std::shared_ptr<const httpp::request>, httpp::responder r) { answer(r, 200, "ok"); });
        s.route_io("GET", "/never", [](std::shared_ptr<const httpp::request>, httpp::responder) { /* kept by nobody */ });
    });
    for (int i = 0; i < 12; ++i) ASSERT_STREQ("ok", httpp::client::fetch(srv.url("/ok")).body.c_str());
    ASSERT_TRUE(after_calls.load() >= 12);
    ASSERT_TRUE(httpp::client::fetch(srv.url("/never")).status != 200); // dropped responder: the connection is closed
}

UTEST(httpp_server_io_threads, a_setting_reaches_every_io_thread) {
    local_server srv([](httpp::server& s) {
        s.set_io_threads(4);
        s.set_max_body(8);
        s.route_io("POST", "/in", [](std::shared_ptr<const httpp::request>, httpp::responder r) { answer(r, 200, "in"); });
    });
    int too_big = 0;
    for (int i = 0; i < 24; ++i) { // new connection each time: spread over the acceptors
        auto res = httpp::client::request(srv.url("/in")).method("POST").data("0123456789abcdef").run();
        if (res.status == 413) ++too_big;
    }
    ASSERT_EQ(24, too_big);
}

UTEST(httpp_server_io_threads, the_server_stops_and_listens_again_with_several_io_threads) {
    httpp::server s;
    s.set_io_threads(3);
    s.route_io("GET", "/x", [](std::shared_ptr<const httpp::request>, httpp::responder r) { answer(r, 200, "x"); });
    for (int round = 0; round < 2; ++round) {
        const int port = s.bind_to_any_port("127.0.0.1");
        ASSERT_TRUE(port > 0);
        std::thread th([&] { s.listen_after_bind(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        ASSERT_STREQ("x", httpp::client::fetch("http://127.0.0.1:" + std::to_string(port) + "/x").body.c_str());
        s.stop();
        th.join();
    }
}

UTEST(httpp_server_io_threads, the_date_header_is_well_formed_when_every_io_thread_answers_at_once) {
    // The Date text is cached per second. The cache must belong to one I/O thread: shared, two threads fill and read it
    // together (a data race that ThreadSanitizer reports, and that can corrupt the string on the very first answers).
    for (int round = 0; round < 20; ++round) {
        local_server srv([](httpp::server& s) {
            s.set_io_threads(4);
            s.route_io("GET", "/d", [](std::shared_ptr<const httpp::request>, httpp::responder r) { answer(r, 200, "x"); });
        });
        std::atomic<int> bad{0};
        std::vector<std::thread> users;
        for (int i = 0; i < 16; ++i) {
            users.emplace_back([&] {
                httpp::client c(srv.url());
                for (int k = 0; k < 20; ++k) {
                    const std::string d = c.get("/d").header("Date"); // "Sat, 10 Oct 2026 12:00:00 GMT"
                    if (d.size() != 29 || d.compare(25, 4, " GMT") != 0 || d[3] != ',') ++bad;
                }
            });
        }
        for (auto& t : users) t.join();
        ASSERT_EQ(0, bad.load());
    }
}

#endif
