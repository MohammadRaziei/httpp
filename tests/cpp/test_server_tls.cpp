// The server over TLS: server::set_tls, https and wss, shutdown, redirects (ADR-0040).
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/middleware.hpp"
#include "httpp/server.hpp"
#include "internal/tls_session.hpp"
#include "local_server.hpp"
#include "test_certs.hpp"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using testutil::local_server;
using namespace std::chrono;

namespace {

// The test certificates as files (the API takes paths), written once per process.
struct cert_files {
    std::string cert, key, other_key;
    cert_files() {
        const auto dir = std::filesystem::temp_directory_path() / ("httpp_tls_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        auto put = [&](const char* name, const char* text) {
            const auto p = dir / name;
            std::ofstream(p, std::ios::binary) << text;
            return p.string();
        };
        cert = put("cert.pem", testcerts::good_cert);
        key = put("key.pem", testcerts::good_key);
        other_key = put("other_key.pem", testcerts::other_key);
    }
};
const cert_files& files() { static cert_files f; return f; }

void tls_site(httpp::server& s) {
    s.set_tls(files().cert, files().key);
    s.get("/who", [](const httpp::request& r, httpp::response& res) { res.body = r.secure ? "secure " + r.scheme() : "plain " + r.scheme(); });
    s.post("/echo", [](const httpp::request& r, httpp::response& res) { res.body = r.body; });
}

std::string https(const local_server& s, const std::string& path = "") { return "https://127.0.0.1:" + std::to_string(s.port) + path; }

httpp::client trusting(const local_server& s) {
    httpp::client c(https(s));
    c.set_ca_file(files().cert);
    return c;
}

// A bare TLS client over a blocking socket, for what the HTTP client cannot do (WebSocket, odd input).
struct tls_raw {
    asio::io_context io;
    asio::ip::tcp::socket sock{io};
    std::shared_ptr<const httpp::detail::tls_context> ctx;
    std::unique_ptr<httpp::detail::tls_session> t;

    bool connect(int port) {
        sock.connect({asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)});
        std::string err;
        ctx = httpp::detail::make_tls_context(true, files().cert, err);
        t = std::make_unique<httpp::detail::tls_session>(ctx, "127.0.0.1");
        char buf[4096];
        for (;;) {
            const auto r = t->handshake();
            const std::string out = t->take_output();
            if (!out.empty()) asio::write(sock, asio::buffer(out));
            if (r == httpp::detail::tls_session::result::ok) return true;
            if (r == httpp::detail::tls_session::result::failed) return false;
            std::error_code ec;
            const std::size_t n = sock.read_some(asio::buffer(buf), ec);
            if (ec) return false;
            t->feed(buf, n);
        }
    }
    void send(const std::string& plain) {
        t->write(plain);
        asio::write(sock, asio::buffer(t->take_output()));
    }
    // Reads until `until` is in what arrived, the peer closes, or `limit` passes.
    std::string recv_until(const std::string& until, milliseconds limit = seconds(3)) {
        std::string plain;
        char buf[4096];
        const auto end = steady_clock::now() + limit;
        sock.non_blocking(true);
        while (steady_clock::now() < end) {
            for (;;) {
                const auto r = t->read(plain);
                if (r == httpp::detail::tls_session::result::ok) continue;
                if (r != httpp::detail::tls_session::result::more) return plain; // closed or failed
                break;
            }
            if (!until.empty() && plain.find(until) != std::string::npos) return plain;
            std::error_code ec;
            const std::size_t n = sock.read_some(asio::buffer(buf), ec);
            if (ec == asio::error::would_block) { std::this_thread::sleep_for(milliseconds(2)); continue; }
            if (ec) return plain;
            t->feed(buf, n);
        }
        return plain;
    }
};

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

} // namespace

UTEST(httpp_server_tls, an_https_request_is_answered_and_the_handler_sees_a_secure_request) {
    local_server srv(tls_site);
    auto r = trusting(srv).get("/who");
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("secure https", r.body.c_str());
}

UTEST(httpp_server_tls, keep_alive_and_a_body_of_many_records_round_trip) {
    local_server srv(tls_site);
    auto c = trusting(srv);
    std::string big(1500 * 1000, 'x');
    for (std::size_t i = 0; i < big.size(); i += 997) big[i] = static_cast<char>('a' + (i % 26));
    for (int i = 0; i < 3; ++i) { // the same session: one TLS connection, several requests
        auto r = c.post("/echo", big);
        ASSERT_EQ(200, r.status);
        ASSERT_TRUE(r.body == big);
    }
    ASSERT_EQ(200, c.get("/who").status);
}

UTEST(httpp_server_tls, a_client_that_does_not_verify_is_served_too_and_one_that_does_refuses_an_unknown_ca) {
    local_server srv(tls_site);
    httpp::client open(https(srv));
    open.set_verify(false);
    ASSERT_EQ(200, open.get("/who").status);
    httpp::client strict(https(srv));
    strict.set_ca_file(files().key); // not a CA that signed it (and not a certificate at all)
    ASSERT_EQ(0, strict.get("/who").status);
}

UTEST(httpp_server_tls, plain_http_and_garbage_on_a_tls_port_are_dropped_and_the_server_keeps_serving) {
    local_server srv(tls_site);
    {
        asio::io_context io;
        asio::ip::tcp::socket s(io);
        s.connect({asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(srv.port)});
        asio::write(s, asio::buffer(std::string("GET /who HTTP/1.1\r\nHost: x\r\n\r\n")));
        std::error_code ec;
        char buf[512];
        s.read_some(asio::buffer(buf), ec); // a TLS alert or a plain close; never an HTTP answer
        ASSERT_TRUE(ec || std::string(buf, 4) != "HTTP");
    }
    {
        asio::io_context io;
        asio::ip::tcp::socket s(io);
        s.connect({asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(srv.port)});
        asio::write(s, asio::buffer(std::string(2000, '\x16')));
    }
    ASSERT_EQ(200, trusting(srv).get("/who").status);
}

UTEST(httpp_server_tls, several_connections_on_several_io_threads_at_once) {
    local_server srv([](httpp::server& s) { s.set_io_threads(2); tls_site(s); });
    std::vector<std::future<int>> all;
    for (int i = 0; i < 12; ++i)
        all.push_back(std::async(std::launch::async, [&srv] { return trusting(srv).get("/who").status; }));
    for (auto& f : all) ASSERT_EQ(200, f.get());
}

UTEST(httpp_server_tls, a_websocket_works_over_tls) {
    local_server srv([](httpp::server& s) {
        s.set_tls(files().cert, files().key);
        httpp::ws_handler h;
        h.on_message = [](const httpp::websocket& ws, std::string_view m, bool) { ws.send_text(m); };
        s.route_ws("/ws", h);
    });
    tls_raw c;
    ASSERT_TRUE(c.connect(srv.port));
    c.send("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    const std::string head = c.recv_until("\r\n\r\n");
    ASSERT_TRUE(contains(head, " 101 "));
    ASSERT_TRUE(contains(head, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    const char frame[] = {'\x81', '\x82', 0, 0, 0, 0, 'h', 'i'}; // a masked text frame "hi" (mask key 0)
    c.send(std::string(frame, sizeof frame));
    const std::string echo = c.recv_until("hi");
    ASSERT_TRUE(contains(echo, std::string("\x81\x02hi", 4)));
}

UTEST(httpp_server_tls, shutdown_lets_an_in_flight_tls_request_finish) {
    std::atomic<bool> started{false};
    local_server srv([&](httpp::server& s) {
        s.set_tls(files().cert, files().key);
        s.get("/slow", [&](const httpp::request&, httpp::response& res) {
            started = true;
            std::this_thread::sleep_for(milliseconds(300));
            res.body = "done";
        });
    });
    std::promise<httpp::response> reply;
    std::thread client([&] { reply.set_value(trusting(srv).get("/slow")); });
    while (!started) std::this_thread::sleep_for(milliseconds(2));
    srv.srv.shutdown(seconds(5));
    client.join();
    auto r = reply.get_future().get();
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("done", r.body.c_str());
}

UTEST(httpp_server_tls, set_tls_reports_a_missing_file_a_garbage_file_and_a_key_that_does_not_match) {
    auto message = [](auto fn) {
        try { fn(); } catch (const std::runtime_error& e) { return std::string(e.what()); }
        return std::string();
    };
    httpp::server s;
    const std::string missing = message([&] { s.set_tls("/no/such/cert.pem", files().key); });
    ASSERT_TRUE(contains(missing, "/no/such/cert.pem"));
    const std::string garbage = message([&] { s.set_tls(files().key, files().key); }); // a key where the certificate belongs
    ASSERT_FALSE(garbage.empty());
    const std::string mismatch = message([&] { s.set_tls(files().cert, files().other_key); });
    ASSERT_TRUE(contains(mismatch, "match"));
    s.set_tls(files().cert, files().key); // and a good pair still works after the failures
}

UTEST(httpp_server_tls, the_https_redirect_sends_plain_requests_to_https_and_lets_secure_ones_through) {
    local_server plain([](httpp::server& s) {
        s.get("/x", [](const httpp::request&, httpp::response& res) { res.body = "x"; });
        httpp::add_https_redirect(s, {false});
    });
    auto r = httpp::client("http://127.0.0.1:" + std::to_string(plain.port)).get("/x?a=1");
    ASSERT_EQ(307, r.status);
    ASSERT_TRUE(contains(r.header("Location"), "https://127.0.0.1:" + std::to_string(plain.port) + "/x?a=1"));

    local_server proxied([](httpp::server& s) {
        s.get("/x", [](const httpp::request&, httpp::response& res) { res.body = "x"; });
        httpp::add_https_redirect(s, {true}); // behind a proxy that terminates TLS and says so
    });
    httpp::client c("http://127.0.0.1:" + std::to_string(proxied.port));
    ASSERT_EQ(200, c.get("/x", {{"X-Forwarded-Proto", "https"}}).status);
    ASSERT_EQ(307, c.get("/x", {{"X-Forwarded-Proto", "http"}}).status);
    local_server untrusted([](httpp::server& s) {
        s.get("/x", [](const httpp::request&, httpp::response& res) { res.body = "x"; });
        httpp::add_https_redirect(s, {false}); // the header means nothing unless the owner trusts the proxy
    });
    ASSERT_EQ(307, httpp::client("http://127.0.0.1:" + std::to_string(untrusted.port)).get("/x", {{"X-Forwarded-Proto", "https"}}).status);

    local_server secure([](httpp::server& s) {
        tls_site(s);
        httpp::add_https_redirect(s, {false});
    });
    ASSERT_EQ(200, trusting(secure).get("/who").status);
}

UTEST(httpp_server_tls, the_www_redirect_of_trusted_host_keeps_https) {
    local_server srv([](httpp::server& s) {
        tls_site(s);
        httpp::add_trusted_host(s, {{"www.example.com"}, true});
    });
    tls_raw c;
    ASSERT_TRUE(c.connect(srv.port));
    c.send("GET /who HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
    const std::string r = c.recv_until("\r\n\r\n");
    ASSERT_TRUE(contains(r, " 307 "));
    ASSERT_TRUE(contains(r, "Location: https://www.example.com/who"));
}
