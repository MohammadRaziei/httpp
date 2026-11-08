// Graceful shutdown: server::shutdown(grace) drains in-flight requests, closes idle connections, then forces the rest (ADR-0038).
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "ws_test_client.hpp"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono;

namespace {

// A server on an ephemeral port that runs listen_after_bind() on its own thread and reports when it returns.
struct run_server {
    httpp::server srv;
    int port = 0;
    std::thread th;
    std::atomic<bool> returned{false};

    explicit run_server(std::function<void(httpp::server&)> setup, std::size_t io_threads = 1) {
        if (io_threads > 1) srv.set_io_threads(io_threads);
        setup(srv);
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); returned = true; });
        std::this_thread::sleep_for(milliseconds(50));
    }
    ~run_server() {
        if (!returned) srv.stop();
        th.join();
    }
    std::string url(const std::string& path = "") const { return "http://127.0.0.1:" + std::to_string(port) + path; }
    bool wait_returned(milliseconds limit) {
        const auto end = steady_clock::now() + limit;
        while (!returned && steady_clock::now() < end) std::this_thread::sleep_for(milliseconds(5));
        return returned;
    }
};

// One raw keep-alive connection: send a request, read the head of the answer.
struct raw_conn {
    asio::io_context io;
    asio::ip::tcp::socket s{io};
    explicit raw_conn(int port) { s.connect({asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)}); }
    void send(const std::string& req) { asio::write(s, asio::buffer(req)); }
    // Reads until the connection ends or `until` shows up; returns what arrived.
    std::string read_until(const std::string& until, milliseconds limit) {
        std::string out;
        const auto end = steady_clock::now() + limit;
        s.non_blocking(true);
        char buf[2048];
        while (steady_clock::now() < end) {
            std::error_code ec;
            std::size_t n = s.read_some(asio::buffer(buf), ec);
            if (n) out.append(buf, n);
            if (!until.empty() && out.find(until) != std::string::npos) return out;
            if (ec == asio::error::would_block) { std::this_thread::sleep_for(milliseconds(2)); continue; }
            if (ec) return out; // closed
        }
        return out;
    }
    bool closed_within(milliseconds limit) {
        const auto end = steady_clock::now() + limit;
        s.non_blocking(true);
        char b;
        while (steady_clock::now() < end) {
            std::error_code ec;
            s.read_some(asio::buffer(&b, 1), ec);
            if (ec && ec != asio::error::would_block) return true;
            std::this_thread::sleep_for(milliseconds(2));
        }
        return false;
    }
};

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

} // namespace

UTEST(httpp_shutdown, an_in_flight_request_finishes_and_is_told_to_close) {
    std::atomic<bool> started{false};
    run_server srv([&](httpp::server& s) {
        s.get("/slow", [&](const httpp::request&, httpp::response& res) {
            started = true;
            std::this_thread::sleep_for(milliseconds(400));
            res.body = "done";
        });
    });
    raw_conn c(srv.port);
    c.send("GET /slow HTTP/1.1\r\nHost: x\r\n\r\n"); // keep-alive request
    while (!started) std::this_thread::sleep_for(milliseconds(2));
    srv.srv.shutdown(seconds(5));
    const std::string answer = c.read_until("done", seconds(3));
    ASSERT_TRUE(contains(answer, "200 OK"));
    ASSERT_TRUE(contains(answer, "done"));
    ASSERT_TRUE(contains(answer, "Connection: close"));
    ASSERT_TRUE(srv.wait_returned(seconds(3)));
}

UTEST(httpp_shutdown, an_idle_keep_alive_connection_is_closed_at_once_and_new_ones_are_refused) {
    run_server srv([](httpp::server& s) {
        s.set_keep_alive_timeout(30);
        s.get("/x", [](const httpp::request&, httpp::response& res) { res.body = "x"; });
    });
    raw_conn c(srv.port);
    c.send("GET /x HTTP/1.1\r\nHost: x\r\n\r\n");
    ASSERT_TRUE(contains(c.read_until("\r\n\r\nx", seconds(2)), "200 OK")); // now idle, keep-alive
    const auto t0 = steady_clock::now();
    srv.srv.shutdown(seconds(10));
    ASSERT_TRUE(c.closed_within(seconds(2)));
    ASSERT_TRUE(srv.wait_returned(seconds(2)));
    ASSERT_LT(duration_cast<milliseconds>(steady_clock::now() - t0).count(), 2000);
    ASSERT_EQ(0, httpp::client::fetch(srv.url("/x")).status); // nobody listens any more
}

UTEST(httpp_shutdown, after_the_grace_period_an_unanswered_request_is_cut) {
    auto stash = std::make_shared<httpp::responder>();
    std::atomic<bool> got{false};
    run_server srv([&](httpp::server& s) {
        s.set_async_timeout(0); // never answer on its own
        s.get_async("/hang", [&](const httpp::request&, httpp::responder r) { *stash = r; got = true; });
    });
    std::promise<int> status;
    std::thread client([&] { status.set_value(httpp::client::fetch(srv.url("/hang")).status); });
    while (!got) std::this_thread::sleep_for(milliseconds(2));
    const auto t0 = steady_clock::now();
    srv.srv.shutdown(milliseconds(300));
    ASSERT_TRUE(srv.wait_returned(seconds(3)));
    const auto took = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    ASSERT_GE(took, 250);   // it waited for the grace period
    ASSERT_LT(took, 2000);  // and not much longer
    client.join();
    ASSERT_EQ(0, status.get_future().get()); // connection cut: no answer
}

UTEST(httpp_shutdown, works_with_several_io_threads_and_a_late_answer_inside_the_grace_period) {
    auto stash = std::make_shared<httpp::responder>();
    std::atomic<bool> got{false};
    run_server srv([&](httpp::server& s) {
        s.get_async("/later", [&](const httpp::request&, httpp::responder r) { *stash = r; got = true; });
    }, 4); // four loops, one connection: most engines have nothing to drain and finish first
    std::promise<httpp::response> reply;
    std::thread client([&] { reply.set_value(httpp::client::fetch(srv.url("/later"))); });
    while (!got) std::this_thread::sleep_for(milliseconds(2));
    srv.srv.shutdown(seconds(5));
    std::this_thread::sleep_for(milliseconds(100));
    ASSERT_FALSE(srv.returned.load()); // still waiting for the answer
    httpp::response ok;
    ok.status = 200;
    ok.body = "late";
    stash->send(std::move(ok));
    ASSERT_TRUE(srv.wait_returned(seconds(3)));
    client.join();
    auto r = reply.get_future().get();
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("late", r.body.c_str());
}

UTEST(httpp_shutdown, a_server_that_is_not_listening_ignores_it_and_stop_still_works_after) {
    httpp::server s;
    s.shutdown(seconds(1)); // no-op
    s.get("/x", [](const httpp::request&, httpp::response& res) { res.body = "x"; });
    run_server srv([](httpp::server& t) { t.get("/x", [](const httpp::request&, httpp::response& res) { res.body = "x"; }); });
    srv.srv.shutdown(seconds(1));
    srv.srv.stop(); // a hard stop during or after a drain is harmless
    ASSERT_TRUE(srv.wait_returned(seconds(2)));
}

UTEST(httpp_shutdown, a_websocket_gets_a_1001_close_frame_and_the_server_ends_when_it_answers) {
    run_server srv([](httpp::server& s) { s.route_ws("/ws", httpp::ws_handler{}); });
    testutil::ws_client c(srv.port);
    ASSERT_EQ(101, testutil::head_status(c.handshake("/ws")));
    srv.srv.shutdown(seconds(10));
    testutil::ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f, milliseconds(2000)));
    ASSERT_EQ(8u, f.op);
    ASSERT_TRUE(f.payload.size() >= 2 && f.payload.substr(0, 2) == testutil::ws_close_body(1001));
    ASSERT_FALSE(srv.returned); // it waits for the answer: the connection is still open
    c.send_frame(8, testutil::ws_close_body(1001));
    ASSERT_TRUE(c.closed_by_peer(milliseconds(2000))); // the server ends the connection once the answer is in
    c.close_socket();                                  // a real client closes its end too; the server waits for that, up to its linger time
    ASSERT_TRUE(srv.wait_returned(milliseconds(2000)));
}

UTEST(httpp_shutdown, a_websocket_that_never_answers_is_cut_when_the_grace_period_is_over) {
    run_server srv([](httpp::server& s) { s.route_ws("/ws", httpp::ws_handler{}); });
    testutil::ws_client c(srv.port);
    ASSERT_EQ(101, testutil::head_status(c.handshake("/ws")));
    const auto t0 = steady_clock::now();
    srv.srv.shutdown(milliseconds(300));
    testutil::ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f, milliseconds(2000)));
    ASSERT_EQ(8u, f.op);
    ASSERT_TRUE(srv.wait_returned(milliseconds(3000)));
    ASSERT_TRUE(c.closed_by_peer(milliseconds(2000)));
    ASSERT_TRUE(steady_clock::now() - t0 >= milliseconds(250)); // it did wait for the peer, it did not cut at once
}

UTEST(httpp_shutdown, a_websocket_handler_sees_the_close_with_the_peers_code) {
    std::atomic<int> code{0};
    run_server srv([&](httpp::server& s) {
        httpp::ws_handler h;
        h.on_close = [&](const httpp::websocket&, std::uint16_t c, std::string_view) { code = c; };
        s.route_ws("/ws", h);
    });
    testutil::ws_client c(srv.port);
    ASSERT_EQ(101, testutil::head_status(c.handshake("/ws")));
    srv.srv.shutdown(seconds(10));
    testutil::ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f, milliseconds(2000)));
    c.send_frame(8, testutil::ws_close_body(1001));
    ASSERT_TRUE(c.closed_by_peer(milliseconds(2000)));
    c.close_socket();
    ASSERT_TRUE(srv.wait_returned(milliseconds(2000)));
    ASSERT_EQ(1001, code.load());
}
