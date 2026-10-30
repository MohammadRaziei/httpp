// server::route_ws end to end: a real engine, a real socket, a bare client (ws_test_client.hpp).
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"
#include "ws_test_client.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using testutil::head_header;
using testutil::head_status;
using testutil::local_server;
using testutil::ws_client;
using testutil::ws_frame_in;
using testutil::ws_close_body;

namespace {

constexpr unsigned op_text = 1, op_binary = 2, op_close = 8, op_ping = 9, op_pong = 10;

httpp::ws_handler echo() {
    httpp::ws_handler h;
    h.on_message = [](const httpp::websocket& ws, std::string_view m, bool binary) {
        if (binary) ws.send_binary(m);
        else ws.send_text(m);
    };
    return h;
}

// The state a test shares with the callbacks.
struct log_state {
    std::mutex mu;
    int opens = 0, closes = 0;
    unsigned close_code = 0;
    std::string close_reason;
    std::thread::id open_thread;
    std::thread::id message_thread;
    httpp::websocket kept;
    bool wait_for_close(int ms = 3000) {
        for (int i = 0; i < ms / 5; ++i) {
            { std::lock_guard<std::mutex> lk(mu); if (closes) return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

void watch(httpp::ws_handler& h, const std::shared_ptr<log_state>& st) {
    auto prev_open = h.on_open;
    h.on_open = [st, prev_open](const httpp::websocket& ws) {
        {
            std::lock_guard<std::mutex> lk(st->mu);
            ++st->opens;
            st->open_thread = std::this_thread::get_id();
            st->kept = ws;
        }
        if (prev_open) prev_open(ws);
    };
    auto prev_msg = h.on_message;
    h.on_message = [st, prev_msg](const httpp::websocket& ws, std::string_view m, bool b) {
        { std::lock_guard<std::mutex> lk(st->mu); st->message_thread = std::this_thread::get_id(); }
        if (prev_msg) prev_msg(ws, m, b);
    };
    auto prev_close = h.on_close;
    h.on_close = [st, prev_close](const httpp::websocket& ws, std::uint16_t code, std::string_view reason) {
        {
            std::lock_guard<std::mutex> lk(st->mu);
            ++st->closes;
            st->close_code = code;
            st->close_reason = std::string(reason);
        }
        if (prev_close) prev_close(ws, code, reason);
    };
}

} // namespace

UTEST(httpp_server_ws, the_handshake_answers_101_with_the_accept_key_of_the_rfc) {
    local_server srv([](httpp::server& s) { s.route_ws("/ws", echo()); });
    ws_client c(srv.port);
    const std::string head = c.handshake("/ws");
    ASSERT_EQ(101, head_status(head));
    ASSERT_TRUE(head_header(head, "Sec-WebSocket-Accept") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    ASSERT_TRUE(head_header(head, "Upgrade") == "websocket");
    ASSERT_TRUE(head_header(head, "Connection") == "Upgrade");
}

UTEST(httpp_server_ws, text_and_binary_messages_are_echoed) {
    local_server srv([](httpp::server& s) { s.route_ws("/ws", echo()); });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    c.send_frame(op_text, "hello");
    c.send_frame(op_binary, std::string("\x00\xff\x01", 3));
    ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f));
    ASSERT_TRUE(f.op == op_text && f.payload == "hello" && f.fin);
    ASSERT_TRUE(c.read_frame(f));
    ASSERT_TRUE(f.op == op_binary && f.payload == std::string("\x00\xff\x01", 3));
}

UTEST(httpp_server_ws, a_large_message_in_many_fragments_and_reads_comes_back_whole) {
    local_server srv([](httpp::server& s) {
        auto h = echo();
        h.max_message = 4u << 20;
        h.max_buffered = 8u << 20;
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    const std::string big(1500000, 'z');
    c.send_frame(op_binary, big.substr(0, 500000), false);
    c.send_frame(0, big.substr(500000, 500000), false);
    c.send_frame(0, big.substr(1000000), true);
    ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f, std::chrono::milliseconds(10000)));
    ASSERT_TRUE(f.op == op_binary);
    ASSERT_TRUE(f.payload == big);
}

UTEST(httpp_server_ws, a_ping_gets_a_pong) {
    local_server srv([](httpp::server& s) { s.route_ws("/ws", echo()); });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    c.send_frame(op_ping, "pp");
    ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f));
    ASSERT_TRUE(f.op == op_pong && f.payload == "pp");
}

UTEST(httpp_server_ws, frames_sent_right_behind_the_upgrade_request_are_not_lost) {
    local_server srv([](httpp::server& s) { s.route_ws("/ws", echo()); });
    ws_client c(srv.port);
    c.send_raw(ws_client::upgrade_request("/ws") + testutil::ws_mask_frame(op_text, "early"));
    ASSERT_EQ(101, head_status(c.read_head()));
    ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f));
    ASSERT_TRUE(f.payload == "early");
}

UTEST(httpp_server_ws, the_closing_handshake_ends_with_the_server_hanging_up) {
    auto st = std::make_shared<log_state>();
    local_server srv([&](httpp::server& s) {
        auto h = echo();
        watch(h, st);
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    c.send_frame(op_close, ws_close_body(1000, "done"));
    ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f));
    ASSERT_TRUE(f.op == op_close && f.payload == ws_close_body(1000, "done"));
    ASSERT_TRUE(c.closed_by_peer());
    ASSERT_TRUE(st->wait_for_close());
    std::lock_guard<std::mutex> lk(st->mu);
    ASSERT_EQ(1000u, st->close_code);
    ASSERT_TRUE(st->close_reason == "done");
}

UTEST(httpp_server_ws, a_client_that_just_disconnects_is_reported_as_1006) {
    auto st = std::make_shared<log_state>();
    local_server srv([&](httpp::server& s) {
        auto h = echo();
        watch(h, st);
        s.route_ws("/ws", h);
    });
    {
        ws_client c(srv.port);
        ASSERT_EQ(101, head_status(c.handshake("/ws")));
    }
    ASSERT_TRUE(st->wait_for_close());
    std::lock_guard<std::mutex> lk(st->mu);
    ASSERT_EQ(1, st->opens);
    ASSERT_EQ(1006u, st->close_code);
}

UTEST(httpp_server_ws, a_protocol_error_closes_with_1002_and_hangs_up) {
    local_server srv([](httpp::server& s) { s.route_ws("/ws", echo()); });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    c.send_raw(testutil::ws_mask_frame(op_text, "x", true, false));  // unmasked
    ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f));
    ASSERT_TRUE(f.op == op_close && f.payload.substr(0, 2) == ws_close_body(1002));
    ASSERT_TRUE(c.closed_by_peer());
}

UTEST(httpp_server_ws, a_message_over_max_message_closes_with_1009) {
    local_server srv([](httpp::server& s) {
        auto h = echo();
        h.max_message = 1000;
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    c.send_frame(op_binary, std::string(2000, 'a'));
    ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f));
    ASSERT_TRUE(f.op == op_close && f.payload.substr(0, 2) == ws_close_body(1009));
    ASSERT_TRUE(c.closed_by_peer());
}

// ---------------------------------------------------------------- the handshake itself

UTEST(httpp_server_ws, a_request_that_is_not_an_upgrade_gets_400) {
    local_server srv([](httpp::server& s) { s.route_ws("/ws", echo()); });
    ASSERT_EQ(400, httpp::client::fetch(srv.url("/ws")).status);
    ws_client no_key(srv.port);
    no_key.send_raw(ws_client::upgrade_request("/ws", "", ""));
    ASSERT_EQ(400, head_status(no_key.read_head()));
    ws_client bad_key(srv.port);
    bad_key.send_raw(ws_client::upgrade_request("/ws", "", "not-a-key"));
    ASSERT_EQ(400, head_status(bad_key.read_head()));
    ws_client no_conn(srv.port);
    no_conn.send_raw("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    ASSERT_EQ(400, head_status(no_conn.read_head()));
}

UTEST(httpp_server_ws, another_protocol_version_gets_426_that_names_version_13) {
    local_server srv([](httpp::server& s) { s.route_ws("/ws", echo()); });
    ws_client c(srv.port);
    c.send_raw(ws_client::upgrade_request("/ws", "", "dGhlIHNhbXBsZSBub25jZQ==", "8"));
    const std::string head = c.read_head();
    ASSERT_EQ(426, head_status(head));
    ASSERT_TRUE(head_header(head, "Sec-WebSocket-Version") == "13");
}

UTEST(httpp_server_ws, header_values_are_matched_as_tokens_without_regard_to_case) {
    local_server srv([](httpp::server& s) { s.route_ws("/ws", echo()); });
    ws_client c(srv.port);
    c.send_raw("GET /ws HTTP/1.1\r\nHost: x\r\nupgrade: WebSocket\r\nconnection: keep-alive, Upgrade\r\n"
               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    ASSERT_EQ(101, head_status(c.read_head()));
}

UTEST(httpp_server_ws, other_methods_and_unknown_paths_are_the_usual_errors) {
    local_server srv([](httpp::server& s) { s.route_ws("/ws", echo()); });
    ASSERT_EQ(405, httpp::client::request(srv.url("/ws")).method("POST").data("x").run().status);
    ws_client c(srv.port);
    c.send_raw(ws_client::upgrade_request("/nope"));
    ASSERT_EQ(404, head_status(c.read_head()));
}

UTEST(httpp_server_ws, on_handshake_can_reject_and_the_rejection_is_what_the_client_sees) {
    local_server srv([](httpp::server& s) {
        auto h = echo();
        h.on_handshake = [](std::shared_ptr<const httpp::request> req, httpp::ws_acceptor acceptor) {
            if (req->header("Origin") == "https://good.example") acceptor.accept();
            else acceptor.reject();
        };
        s.route_ws("/ws", h);
    });
    ws_client bad(srv.port);
    bad.send_raw(ws_client::upgrade_request("/ws", "Origin: https://evil.example\r\n"));
    ASSERT_EQ(403, head_status(bad.read_head()));
    ws_client good(srv.port);
    good.send_raw(ws_client::upgrade_request("/ws", "Origin: https://good.example\r\n"));
    ASSERT_EQ(101, head_status(good.read_head()));
}

UTEST(httpp_server_ws, on_handshake_can_choose_a_subprotocol_but_only_an_offered_one) {
    local_server srv([](httpp::server& s) {
        auto h = echo();
        h.on_handshake = [](std::shared_ptr<const httpp::request> req, httpp::ws_acceptor acceptor) {
            try {
                acceptor.accept({{"Sec-WebSocket-Protocol", req->query_param("pick")}});
            } catch (const std::invalid_argument&) {  // not offered: the client has been answered with a 500
            }
        };
        s.route_ws("/ws", h);
    });
    ws_client ok(srv.port);
    ok.send_raw(ws_client::upgrade_request("/ws?pick=chat", "Sec-WebSocket-Protocol: superchat, chat\r\n"));
    const std::string head = ok.read_head();
    ASSERT_EQ(101, head_status(head));
    ASSERT_TRUE(head_header(head, "Sec-WebSocket-Protocol") == "chat");
    ws_client bad(srv.port);
    bad.send_raw(ws_client::upgrade_request("/ws?pick=other", "Sec-WebSocket-Protocol: chat\r\n"));
    ASSERT_EQ(500, head_status(bad.read_head()));
}

UTEST(httpp_server_ws, the_upgrade_request_is_visible_to_the_handlers) {
    std::mutex mu;
    std::string seen;
    local_server srv([&](httpp::server& s) {
        httpp::ws_handler h;
        h.on_open = [&](const httpp::websocket& ws) {
            std::lock_guard<std::mutex> lk(mu);
            seen = ws.handshake().path + "|" + ws.handshake().path_param("room") + "|" + ws.handshake().query_param("token") + "|" +
                   ws.handshake().cookie("sid");
        };
        s.route_ws("/rooms/:room", h);
    });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/rooms/lobby?token=t1", "Cookie: sid=abc\r\n")));
    for (int i = 0; i < 100; ++i) {
        { std::lock_guard<std::mutex> lk(mu); if (!seen.empty()) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::lock_guard<std::mutex> lk(mu);
    ASSERT_TRUE(seen == "/rooms/lobby|lobby|t1|abc");
}

UTEST(httpp_server_ws, a_before_hook_that_refuses_stops_the_upgrade) {
    local_server srv([](httpp::server& s) {
        s.route_ws("/ws", echo());
        s.before([](const httpp::request& r, httpp::response& res) {
            if (r.header("Authorization").empty()) { res.status = 401; return true; }
            return false;
        });
    });
    ws_client denied(srv.port);
    ASSERT_EQ(401, head_status(denied.handshake("/ws")));
    ws_client allowed(srv.port);
    ASSERT_EQ(101, head_status(allowed.handshake("/ws", "Authorization: x\r\n")));
    ws_frame_in f;
    allowed.send_frame(op_text, "after before");
    ASSERT_TRUE(allowed.read_frame(f));
    ASSERT_TRUE(f.payload == "after before");
}

UTEST(httpp_server_ws, normal_routes_keep_working_next_to_a_websocket_route) {
    local_server srv([](httpp::server& s) {
        s.route_ws("/ws", echo());
        s.get("/hello", [](const httpp::request&, httpp::response& res) { res.body = "hi"; });
    });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    ASSERT_TRUE(httpp::client::fetch(srv.url("/hello")).body == "hi");
}

// ---------------------------------------------------------------- threads

UTEST(httpp_server_ws, the_callbacks_run_on_the_io_thread) {
    auto st = std::make_shared<log_state>();
    local_server srv([&](httpp::server& s) {
        auto h = echo();
        watch(h, st);
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    c.send_frame(op_text, "x");
    ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f));
    std::lock_guard<std::mutex> lk(st->mu);
    ASSERT_TRUE(st->open_thread == srv.th.get_id());
    ASSERT_TRUE(st->message_thread == srv.th.get_id());
}

UTEST(httpp_server_ws, another_thread_can_send_and_close) {
    auto st = std::make_shared<log_state>();
    local_server srv([&](httpp::server& s) {
        httpp::ws_handler h;
        watch(h, st);
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    httpp::websocket ws;
    for (int i = 0; i < 200 && !ws; ++i) {
        { std::lock_guard<std::mutex> lk(st->mu); ws = st->kept; }
        if (!ws) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(static_cast<bool>(ws));
    std::thread t([&] {
        for (int i = 0; i < 50; ++i) ws.send_text("m" + std::to_string(i));
        ws.close(1001, "bye");
    });
    t.join();
    ws_frame_in f;
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(c.read_frame(f));
        ASSERT_TRUE(f.op == op_text && f.payload == "m" + std::to_string(i));
    }
    ASSERT_TRUE(c.read_frame(f));
    ASSERT_TRUE(f.op == op_close && f.payload == ws_close_body(1001, "bye"));
    c.send_frame(op_close, ws_close_body(1001));
    ASSERT_TRUE(c.closed_by_peer());
    ASSERT_TRUE(st->wait_for_close());
    ASSERT_FALSE(ws.is_open());
    ASSERT_TRUE(ws.send_text("late") == httpp::websocket::status::closed);
}

UTEST(httpp_server_ws, a_close_sent_from_on_open_reaches_the_peer_and_the_server_stays_alive) {
    local_server srv([](httpp::server& s) {
        httpp::ws_handler h;
        h.on_open = [](const httpp::websocket& ws) { ws.close(1000, "now"); };
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f));
    ASSERT_TRUE(f.op == op_close);
    ASSERT_TRUE(httpp::client::fetch(srv.url("/nothing")).status == 404);  // the server is alive
}

UTEST(httpp_server_ws, several_io_threads_each_serve_their_connections) {
    local_server srv([](httpp::server& s) {
        s.set_io_threads(3);
        s.route_ws("/ws", echo());
    });
    std::vector<std::unique_ptr<ws_client>> clients;
    for (int i = 0; i < 9; ++i) {
        clients.push_back(std::make_unique<ws_client>(srv.port));
        ASSERT_EQ(101, head_status(clients.back()->handshake("/ws")));
    }
    for (int round = 0; round < 3; ++round) {
        for (std::size_t i = 0; i < clients.size(); ++i) clients[i]->send_frame(op_text, "c" + std::to_string(i));
        for (std::size_t i = 0; i < clients.size(); ++i) {
            ws_frame_in f;
            ASSERT_TRUE(clients[i]->read_frame(f));
            ASSERT_TRUE(f.payload == "c" + std::to_string(i));
        }
    }
}

// ---------------------------------------------------------------- backpressure and limits

UTEST(httpp_server_ws, a_slow_reader_makes_send_report_full_and_on_drain_says_when_to_go_on) {
    std::atomic<int> accepted{0};
    std::atomic<int> refused{0};
    std::atomic<int> drains{0};
    std::atomic<std::size_t> peak{0};
    local_server srv([&](httpp::server& s) {
        httpp::ws_handler h;
        h.max_buffered = 256 * 1024;
        h.on_open = [&](const httpp::websocket& ws) {
            const std::string chunk(64 * 1024, 'c');
            for (int i = 0; i < 40; ++i) {
                if (ws.send_binary(chunk) == httpp::websocket::status::ok) ++accepted;
                else { ++refused; break; }
                peak = std::max<std::size_t>(peak, ws.buffered_amount());
            }
        };
        h.on_drain = [&](const httpp::websocket& ws) {
            ++drains;
            ws.send_text("drained");
        };
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    ws_frame_in f;
    int binaries = 0;
    for (;;) {
        ASSERT_TRUE(c.read_frame(f, std::chrono::milliseconds(5000)));
        if (f.op == op_text) break;
        ++binaries;
    }
    ASSERT_TRUE(f.payload == "drained");
    ASSERT_EQ(1, refused.load());
    ASSERT_EQ(accepted.load(), binaries);
    ASSERT_TRUE(accepted.load() >= 4 && accepted.load() <= 6);   // the queue held about 256 KiB, not all 40 chunks
    ASSERT_TRUE(peak.load() <= 256 * 1024 + 70 * 1024);
    ASSERT_EQ(1, drains.load());
}

UTEST(httpp_server_ws, a_peer_that_never_reads_is_dropped_by_the_write_timeout) {
    auto st = std::make_shared<log_state>();
    local_server srv([&](httpp::server& s) {
        s.set_write_timeout(1);
        httpp::ws_handler h;
        h.max_buffered = 64u << 20;
        watch(h, st);
        auto prev = h.on_open;
        h.on_open = [prev](const httpp::websocket& ws) {
            prev(ws);
            const std::string chunk(1u << 20, 'x');
            for (int i = 0; i < 64; ++i) ws.send_binary(chunk);   // far more than the socket buffers hold
        };
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    // never read: the server must give up by itself
    ASSERT_TRUE(st->wait_for_close(8000));
    std::lock_guard<std::mutex> lk(st->mu);
    ASSERT_EQ(1006u, st->close_code);
}

UTEST(httpp_server_ws, a_quiet_peer_is_pinged_and_a_dead_one_is_closed) {
    auto st = std::make_shared<log_state>();
    local_server srv([&](httpp::server& s) {
        httpp::ws_handler h;
        h.ping_interval = 1;
        watch(h, st);
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(f.op == op_ping);
    c.send_frame(op_pong, f.payload);                              // alive: stays open through the next interval
    ASSERT_TRUE(c.read_frame(f, std::chrono::milliseconds(3000)));
    ASSERT_TRUE(f.op == op_ping);                                  // ...and gets pinged again
    ASSERT_TRUE(c.read_frame(f, std::chrono::milliseconds(3000))); // now silent: closed with 1001
    ASSERT_TRUE(f.op == op_close && f.payload.substr(0, 2) == ws_close_body(1001));
    ASSERT_TRUE(c.closed_by_peer());
}

UTEST(httpp_server_ws, stopping_the_server_ends_open_connections_and_reports_1006) {
    auto st = std::make_shared<log_state>();
    auto srv = std::make_unique<local_server>([&](httpp::server& s) {
        auto h = echo();
        watch(h, st);
        s.route_ws("/ws", h);
    });
    ws_client c(srv->port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    srv.reset();
    ASSERT_TRUE(c.closed_by_peer());
    std::lock_guard<std::mutex> lk(st->mu);
    ASSERT_EQ(1, st->closes);
    ASSERT_EQ(1006u, st->close_code);
}

UTEST(httpp_server_ws, pausing_reading_stops_messages_until_resumed) {
    std::atomic<int> got{0};
    auto st = std::make_shared<log_state>();
    local_server srv([&](httpp::server& s) {
        httpp::ws_handler h;
        h.on_message = [&](const httpp::websocket& ws, std::string_view, bool) {
            if (++got == 1) ws.pause_reading();
        };
        watch(h, st);
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws")));
    c.send_frame(op_text, "one");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    c.send_frame(op_text, "two");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_EQ(1, got.load());                                      // "two" waits in the socket
    httpp::websocket ws;
    { std::lock_guard<std::mutex> lk(st->mu); ws = st->kept; }
    ws.resume_reading();
    for (int i = 0; i < 100 && got.load() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(2, got.load());
}

UTEST(httpp_server_ws, the_connection_limit_counts_websocket_connections) {
    local_server srv([](httpp::server& s) {
        s.set_max_connections(1);
        s.route_ws("/ws", echo());
    });
    ws_client first(srv.port);
    ASSERT_EQ(101, head_status(first.handshake("/ws")));
    ws_client second(srv.port);
    ASSERT_EQ(503, head_status(second.read_head()));
}

// ---------------------------------------------------------------- the answer to the upgrade may come later, from anywhere

UTEST(httpp_server_ws, the_acceptor_can_answer_later_from_another_thread) {
    std::mutex mu;
    httpp::ws_acceptor pending;
    std::atomic<bool> have{false};
    auto st = std::make_shared<log_state>();
    local_server srv([&](httpp::server& s) {
        auto h = echo();
        watch(h, st);
        h.on_handshake = [&](std::shared_ptr<const httpp::request>, httpp::ws_acceptor acceptor) {
            std::lock_guard<std::mutex> lk(mu);
            pending = acceptor;
            have = true;
        };
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    c.send_raw(ws_client::upgrade_request("/ws"));
    for (int i = 0; i < 200 && !have; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(have.load());
    ASSERT_TRUE(c.read_head(std::chrono::milliseconds(200)).empty());       // nothing yet: the client waits
    std::thread t([&] { httpp::ws_acceptor a; { std::lock_guard<std::mutex> lk(mu); a = pending; } a.accept(); });
    t.join();
    ASSERT_EQ(101, head_status(c.read_head()));
    c.send_frame(op_text, "late but fine");
    ws_frame_in f;
    ASSERT_TRUE(c.read_frame(f));
    ASSERT_TRUE(f.payload == "late but fine");
    std::lock_guard<std::mutex> lk(st->mu);
    ASSERT_EQ(1, st->opens);
}

UTEST(httpp_server_ws, the_acceptor_can_reject_later_with_its_own_response) {
    httpp::ws_acceptor pending;
    std::mutex mu;
    std::atomic<bool> have{false};
    local_server srv([&](httpp::server& s) {
        auto h = echo();
        h.on_handshake = [&](std::shared_ptr<const httpp::request>, httpp::ws_acceptor acceptor) {
            std::lock_guard<std::mutex> lk(mu);
            pending = acceptor;
            have = true;
        };
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    c.send_raw(ws_client::upgrade_request("/ws"));
    for (int i = 0; i < 200 && !have; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::thread t([&] {
        httpp::response res;
        res.status = 401;
        res.body = "who are you";
        std::lock_guard<std::mutex> lk(mu);
        pending.reject(res);
        pending.accept();   // too late: the first answer counts
    });
    t.join();
    const std::string head = c.read_head();
    ASSERT_EQ(401, head_status(head));
}

UTEST(httpp_server_ws, an_acceptor_that_is_dropped_closes_the_connection_without_an_answer) {
    local_server srv([](httpp::server& s) {
        auto h = echo();
        h.on_handshake = [](std::shared_ptr<const httpp::request>, httpp::ws_acceptor) {};  // forgets it
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    c.send_raw(ws_client::upgrade_request("/ws"));
    ASSERT_TRUE(c.closed_by_peer());
}

UTEST(httpp_server_ws, an_acceptor_that_is_never_answered_gets_the_async_timeout) {
    httpp::ws_acceptor keep;
    std::mutex mu;
    local_server srv([&](httpp::server& s) {
        s.set_async_timeout(1);
        auto h = echo();
        h.on_handshake = [&](std::shared_ptr<const httpp::request>, httpp::ws_acceptor a) { std::lock_guard<std::mutex> lk(mu); keep = a; };
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    c.send_raw(ws_client::upgrade_request("/ws"));
    ASSERT_EQ(504, head_status(c.read_head(std::chrono::milliseconds(5000))));
    std::lock_guard<std::mutex> lk(mu);
    keep.accept();   // after the 504: does nothing
}

UTEST(httpp_server_ws, a_handler_that_throws_in_on_handshake_gets_a_500) {
    local_server srv([](httpp::server& s) {
        auto h = echo();
        h.on_handshake = [](std::shared_ptr<const httpp::request>, httpp::ws_acceptor) { throw std::runtime_error("no"); };
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    c.send_raw(ws_client::upgrade_request("/ws"));
    ASSERT_EQ(500, head_status(c.read_head()));
}

UTEST(httpp_server_ws, the_user_pointer_given_to_accept_reaches_every_callback) {
    std::mutex mu;
    std::string seen;
    local_server srv([&](httpp::server& s) {
        httpp::ws_handler h;
        h.on_handshake = [](std::shared_ptr<const httpp::request> req, httpp::ws_acceptor acceptor) {
            acceptor.accept({}, std::make_shared<std::string>("conn-for-" + req->query_param("who")));
        };
        h.on_message = [&](const httpp::websocket& ws, std::string_view m, bool) {
            std::lock_guard<std::mutex> lk(mu);
            seen = *static_cast<std::string*>(ws.user().get()) + ":" + std::string(m);
        };
        s.route_ws("/ws", h);
    });
    ws_client c(srv.port);
    ASSERT_EQ(101, head_status(c.handshake("/ws?who=ann")));
    c.send_frame(op_text, "hi");
    for (int i = 0; i < 200; ++i) {
        { std::lock_guard<std::mutex> lk(mu); if (!seen.empty()) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::lock_guard<std::mutex> lk(mu);
    ASSERT_TRUE(seen == "conn-for-ann:hi");
}

UTEST(httpp_server_ws, a_client_that_left_before_the_acceptor_answered_still_gets_its_close_reported) {
    auto st = std::make_shared<log_state>();
    httpp::ws_acceptor pending;
    std::mutex mu;
    std::atomic<bool> have{false};
    local_server srv([&](httpp::server& s) {
        auto h = echo();
        watch(h, st);
        h.on_handshake = [&](std::shared_ptr<const httpp::request>, httpp::ws_acceptor a) { std::lock_guard<std::mutex> lk(mu); pending = a; have = true; };
        s.route_ws("/ws", h);
    });
    {
        ws_client c(srv.port);
        c.send_raw(ws_client::upgrade_request("/ws"));
        for (int i = 0; i < 200 && !have; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }                                                         // the client is gone
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // The engine does not read while a request waits for its answer, so it has not noticed that the client left: the accepted connection
    // opens, and the first read finds the end. Either way the close is reported once, as 1006.
    { std::lock_guard<std::mutex> lk(mu); pending.accept(); }
    ASSERT_TRUE(st->wait_for_close());
    std::lock_guard<std::mutex> lk(st->mu);
    ASSERT_TRUE(st->opens <= 1);
    ASSERT_EQ(1, st->closes);
    ASSERT_EQ(1006u, st->close_code);
}
