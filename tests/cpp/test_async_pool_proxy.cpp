// The async engine's keep-alive pool and its HTTP proxy support, against raw Asio servers that can
// misbehave on purpose (hang up on a reused connection, refuse CONNECT, ...).
#include "utest/utest.h"
#include "internal/async_http.hpp"
#include "tls_test_server.hpp"

#include <chrono>
#include <future>
#include <thread>

using httpp::detail::async_http;
using httpp::detail::async_job;
using asio::ip::tcp;
using testtls::tls_server;
using testtls::ok_response;
using testtls::keep_response;
using testtls::write_pem;
using namespace std::chrono_literals;

namespace {

// Blocking HTTP/1.1 server, one thread per connection, with knobs for bad behaviour.
struct http_test_server {
    asio::io_context io;
    tcp::acceptor acc{io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
    std::thread th;
    std::mutex mu;
    std::vector<std::thread> workers;
    std::atomic<bool> stopping{false};
    std::atomic<int> connections{0}, requests{0}, open_now{0};
    bool silent_close_after_response = false; // hang up after answering, without saying Connection: close
    bool say_connection_close = false;        // answer with Connection: close and hang up
    int drop_request_number = 0;              // on each connection, hang up instead of answering this request (1-based)
    std::string last_head;

    int port() { return acc.local_endpoint().port(); }
    http_test_server() { th = std::thread([this] { loop(); }); }
    ~http_test_server() {
        stopping = true;
        { tcp::socket knock(io); std::error_code ec; knock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port())), ec); }
        if (th.joinable()) th.join();
        for (auto& w : workers) w.join();
    }
    void loop() {
        while (!stopping) {
            auto s = std::make_shared<tcp::socket>(io);
            std::error_code ec;
            acc.accept(*s, ec);
            if (ec || stopping) return;
            ++connections;
            std::lock_guard<std::mutex> lk(mu);
            workers.emplace_back([this, s] { serve(s); });
        }
    }
    void serve(std::shared_ptr<tcp::socket> s) {
        ++open_now;
        asio::streambuf buf;
        std::error_code ec;
        for (int n = 1;; ++n) {
            const std::size_t head_end = asio::read_until(*s, buf, "\r\n\r\n", ec);
            if (ec) break;
            std::string head(asio::buffers_begin(buf.data()), asio::buffers_begin(buf.data()) + head_end);
            buf.consume(head_end);
            const auto cl = head.find("Content-Length: ");
            if (cl != std::string::npos) {
                std::size_t len = std::stoul(head.substr(cl + 16));
                if (buf.size() < len) asio::read(*s, buf, asio::transfer_exactly(len - buf.size()), ec);
                buf.consume(len);
            }
            ++requests;
            { std::lock_guard<std::mutex> lk(mu); last_head = head; }
            if (n == drop_request_number) break; // hang up without a word
            const std::string body = "reply " + std::to_string(n);
            std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
            if (say_connection_close) resp += "Connection: close\r\n";
            resp += "\r\n" + body;
            asio::write(*s, asio::buffer(resp), ec);
            if (ec || say_connection_close || silent_close_after_response) break;
        }
        s->shutdown(tcp::socket::shutdown_both, ec);
        --open_now;
    }
};

struct proxy_test_server {
    asio::io_context io;
    tcp::acceptor acc{io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
    std::thread th;
    std::mutex mu;
    std::vector<std::thread> workers;
    std::atomic<bool> stopping{false};
    int refuse_connect_with = 0;   // e.g. 407
    std::string first_line, proxy_auth;
    std::atomic<int> tunnels{0};

    int port() { return acc.local_endpoint().port(); }
    proxy_test_server() { th = std::thread([this] { loop(); }); }
    ~proxy_test_server() {
        stopping = true;
        { tcp::socket knock(io); std::error_code ec; knock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port())), ec); }
        if (th.joinable()) th.join();
        for (auto& w : workers) w.join();
    }
    void loop() {
        while (!stopping) {
            auto s = std::make_shared<tcp::socket>(io);
            std::error_code ec;
            acc.accept(*s, ec);
            if (ec || stopping) return;
            std::lock_guard<std::mutex> lk(mu);
            workers.emplace_back([this, s] { serve(s); });
        }
    }
    static void pump(tcp::socket& from, tcp::socket& to) {
        std::error_code ec;
        char buf[8192];
        for (;;) {
            std::size_t n = from.read_some(asio::buffer(buf), ec);
            if (ec || n == 0) break;
            asio::write(to, asio::buffer(buf, n), ec);
            if (ec) break;
        }
        to.shutdown(tcp::socket::shutdown_send, ec);
    }
    void serve(std::shared_ptr<tcp::socket> s) {
        asio::streambuf buf;
        std::error_code ec;
        const std::size_t n = asio::read_until(*s, buf, "\r\n\r\n", ec);
        if (ec) return;
        const std::string head(asio::buffers_begin(buf.data()), asio::buffers_begin(buf.data()) + n);
        const std::string line = head.substr(0, head.find("\r\n"));
        const auto pa = head.find("Proxy-Authorization: ");
        {
            std::lock_guard<std::mutex> lk(mu);
            first_line = line;
            if (pa != std::string::npos) proxy_auth = head.substr(pa + 21, head.find("\r\n", pa) - pa - 21);
        }
        if (line.rfind("CONNECT ", 0) == 0) {
            if (refuse_connect_with) {
                const std::string r = "HTTP/1.1 " + std::to_string(refuse_connect_with) + " Nope\r\nContent-Length: 0\r\n\r\n";
                asio::write(*s, asio::buffer(r), ec);
                return;
            }
            const std::string authority = line.substr(8, line.find(' ', 8) - 8);
            const auto colon = authority.rfind(':');
            tcp::socket up(io);
            up.connect(tcp::endpoint(asio::ip::make_address(authority.substr(0, colon)), static_cast<unsigned short>(std::stoi(authority.substr(colon + 1)))), ec);
            if (ec) { asio::write(*s, asio::buffer(std::string("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")), ec); return; }
            ++tunnels;
            asio::write(*s, asio::buffer(std::string("HTTP/1.1 200 Connection established\r\n\r\n")), ec);
            std::thread back([&] { pump(up, *s); });
            pump(*s, up);
            back.join();
            return;
        }
        const std::string body = "proxied";
        const std::string r = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        asio::write(*s, asio::buffer(r), ec);
    }
};

std::future<httpp::response> run(async_http& eng, async_job job) {
    auto p = std::make_shared<std::promise<httpp::response>>();
    auto f = p->get_future();
    eng.submit(std::move(job), [p](httpp::response r) { p->set_value(std::move(r)); });
    return f;
}

httpp::response run_now(async_http& eng, async_job job) {
    auto f = run(eng, std::move(job));
    if (f.wait_for(20s) != std::future_status::ready) {
        httpp::response timeout;
        timeout.error = httpp::error_kind::timeout;
        return timeout;
    }
    return f.get();
}

async_job job_for(int port, bool keep_alive, const std::string& method = "GET") {
    async_job j;
    j.host = "127.0.0.1";
    j.port = port;
    j.method = method;
    j.keep_alive = keep_alive;
    j.timeout_sec = 15;
    return j;
}

bool wait_until(const std::function<bool()>& cond, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return cond();
}

} // namespace

// ------------------------------------------------------------------ keep-alive pool

UTEST(async_pool, keep_alive_reuses_one_connection_for_many_requests) {
    http_test_server srv;
    async_http eng;
    for (int i = 1; i <= 5; ++i) {
        auto r = run_now(eng, job_for(srv.port(), true));
        ASSERT_EQ(200, r.status);
        ASSERT_TRUE(r.body == "reply " + std::to_string(i)); // the server's per-connection counter proves reuse
    }
    ASSERT_EQ(1, srv.connections.load());
    ASSERT_EQ(5, srv.requests.load());
}

UTEST(async_pool, without_keep_alive_every_request_gets_its_own_connection) {
    http_test_server srv;
    async_http eng;
    for (int i = 0; i < 3; ++i) ASSERT_EQ(200, run_now(eng, job_for(srv.port(), false)).status);
    ASSERT_EQ(3, srv.connections.load());
}

UTEST(async_pool, a_response_that_says_connection_close_is_not_reused) {
    http_test_server srv;
    srv.say_connection_close = true;
    async_http eng;
    for (int i = 0; i < 3; ++i) ASSERT_EQ(200, run_now(eng, job_for(srv.port(), true)).status);
    ASSERT_EQ(3, srv.connections.load());
}

UTEST(async_pool, a_connection_the_server_closed_while_idle_is_dropped_from_the_pool) {
    http_test_server srv;
    srv.silent_close_after_response = true; // hangs up right after answering, saying nothing
    async_http eng;
    ASSERT_EQ(200, run_now(eng, job_for(srv.port(), true)).status);
    std::this_thread::sleep_for(150ms); // the idle watch sees the hang-up
    auto r = run_now(eng, job_for(srv.port(), true));
    ASSERT_EQ(200, r.status);
    ASSERT_FALSE(r.failed());
    ASSERT_EQ(2, srv.connections.load());
}

UTEST(async_pool, a_reused_connection_that_dies_on_a_get_is_retried_once_on_a_fresh_one) {
    http_test_server srv;
    srv.drop_request_number = 2; // on every connection the 2nd request gets no answer
    async_http eng;
    ASSERT_EQ(200, run_now(eng, job_for(srv.port(), true)).status);
    auto r = run_now(eng, job_for(srv.port(), true)); // reused connection: dropped; retried on connection 2
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(r.body == "reply 1");
    ASSERT_EQ(2, srv.connections.load());
}

UTEST(async_pool, a_post_on_a_dying_reused_connection_is_not_retried) {
    http_test_server srv;
    srv.drop_request_number = 2;
    async_http eng;
    ASSERT_EQ(200, run_now(eng, job_for(srv.port(), true, "POST")).status);
    auto r = run_now(eng, job_for(srv.port(), true, "POST")); // might have been processed: never repeat a POST
    ASSERT_EQ(0, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::connection);
    ASSERT_EQ(1, srv.connections.load());
}

UTEST(async_pool, a_fresh_connection_that_fails_is_not_retried) {
    http_test_server srv;
    srv.drop_request_number = 1; // even the first request on a connection gets no answer
    async_http eng;
    auto r = run_now(eng, job_for(srv.port(), true));
    ASSERT_EQ(0, r.status);
    ASSERT_EQ(1, srv.connections.load());
}

UTEST(async_pool, tls_connections_are_reused_without_a_new_handshake) {
    tls_server srv(testcerts::good_cert, testcerts::good_key, [](const std::string&) { return keep_response("secure"); });
    srv.keep_alive = true;
    async_http eng;
    const std::string ca = write_pem(testcerts::good_cert, "good");
    for (int i = 0; i < 4; ++i) {
        auto j = job_for(srv.port(), true);
        j.tls = true;
        j.ca_file = ca;
        auto r = run_now(eng, std::move(j));
        ASSERT_EQ(200, r.status);
        ASSERT_TRUE(r.body == "secure");
    }
    ASSERT_EQ(1, srv.handshakes_done.load());
    ASSERT_EQ(4, srv.requests_served.load());
}

UTEST(async_pool, different_tls_settings_do_not_share_a_connection) {
    tls_server srv(testcerts::good_cert, testcerts::good_key, [](const std::string&) { return keep_response("x"); });
    srv.keep_alive = true;
    async_http eng;
    auto a = job_for(srv.port(), true);
    a.tls = true;
    a.ca_file = write_pem(testcerts::good_cert, "good");
    ASSERT_EQ(200, run_now(eng, a).status);
    auto b = job_for(srv.port(), true);
    b.tls = true;
    b.verify = false; // a connection that skipped verification must not serve a verifying request
    ASSERT_EQ(200, run_now(eng, b).status);
    ASSERT_EQ(2, srv.handshakes_done.load());
}

UTEST(async_pool, concurrent_requests_use_separate_connections_and_all_return_to_the_pool) {
    http_test_server srv;
    async_http eng;
    std::vector<std::future<httpp::response>> fs;
    for (int i = 0; i < 6; ++i) fs.push_back(run(eng, job_for(srv.port(), true)));
    for (auto& f : fs) { ASSERT_TRUE(f.wait_for(20s) == std::future_status::ready); ASSERT_EQ(200, f.get().status); }
    const int first = srv.connections.load();
    ASSERT_TRUE(first >= 1 && first <= 6);
    for (int i = 0; i < 6; ++i) ASSERT_EQ(200, run_now(eng, job_for(srv.port(), true)).status);
    ASSERT_EQ(first, srv.connections.load()); // all six later requests reused pooled connections
}

UTEST(async_pool, destroying_the_engine_closes_pooled_connections) {
    http_test_server srv;
    {
        async_http eng;
        ASSERT_EQ(200, run_now(eng, job_for(srv.port(), true)).status);
        ASSERT_EQ(1, srv.open_now.load());
    }
    ASSERT_TRUE(wait_until([&] { return srv.open_now.load() == 0; }));
}

// ------------------------------------------------------------------ proxy

UTEST(async_proxy, plain_http_goes_through_the_proxy_in_absolute_form) {
    proxy_test_server proxy;
    async_http eng;
    async_job j;
    j.host = "origin.invalid"; // not resolvable: only reachable through the proxy
    j.port = 8080;
    j.target = "/a/b?x=1";
    j.proxy_host = "127.0.0.1";
    j.proxy_port = proxy.port();
    j.proxy_authorization = "Basic dXNlcjpwYXNz";
    j.timeout_sec = 15;
    auto r = run_now(eng, std::move(j));
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(r.body == "proxied");
    std::lock_guard<std::mutex> lk(proxy.mu);
    ASSERT_TRUE(proxy.first_line == "GET http://origin.invalid:8080/a/b?x=1 HTTP/1.1");
    ASSERT_TRUE(proxy.proxy_auth == "Basic dXNlcjpwYXNz");
}

UTEST(async_proxy, https_is_tunnelled_with_connect) {
    tls_server origin(testcerts::good_cert, testcerts::good_key, [](const std::string&) { return ok_response("through the tunnel"); });
    proxy_test_server proxy;
    async_http eng;
    auto j = job_for(origin.port(), false);
    j.tls = true;
    j.ca_file = write_pem(testcerts::good_cert, "good");
    j.proxy_host = "127.0.0.1";
    j.proxy_port = proxy.port();
    j.proxy_authorization = "Basic dXNlcjpwYXNz";
    auto r = run_now(eng, std::move(j));
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(r.body == "through the tunnel");
    ASSERT_EQ(1, proxy.tunnels.load());
    std::lock_guard<std::mutex> lk(proxy.mu);
    ASSERT_TRUE(proxy.first_line == "CONNECT 127.0.0.1:" + std::to_string(origin.port()) + " HTTP/1.1");
    ASSERT_TRUE(proxy.proxy_auth == "Basic dXNlcjpwYXNz");
}

UTEST(async_proxy, the_certificate_is_still_checked_through_a_tunnel) {
    tls_server origin(testcerts::other_cert, testcerts::other_key, [](const std::string&) { return ok_response("never"); });
    proxy_test_server proxy;
    async_http eng;
    auto j = job_for(origin.port(), false);
    j.tls = true;
    j.ca_file = write_pem(testcerts::other_cert, "other"); // right chain, wrong name for 127.0.0.1
    j.proxy_host = "127.0.0.1";
    j.proxy_port = proxy.port();
    auto r = run_now(eng, std::move(j));
    ASSERT_TRUE(r.error == httpp::error_kind::tls);
}

UTEST(async_proxy, a_proxy_that_refuses_connect_is_a_connection_error_with_the_status) {
    proxy_test_server proxy;
    proxy.refuse_connect_with = 407;
    async_http eng;
    auto j = job_for(1, false);
    j.tls = true;
    j.proxy_host = "127.0.0.1";
    j.proxy_port = proxy.port();
    auto r = run_now(eng, std::move(j));
    ASSERT_EQ(0, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::connection);
    ASSERT_TRUE(r.error_message.find("407") != std::string::npos);
    ASSERT_TRUE(r.error_message.find("authentication") != std::string::npos);
}

UTEST(async_proxy, an_unreachable_proxy_is_a_connection_error) {
    int dead_port;
    { proxy_test_server p; dead_port = p.port(); }
    async_http eng;
    async_job j;
    j.host = "origin.invalid";
    j.port = 80;
    j.proxy_host = "127.0.0.1";
    j.proxy_port = dead_port;
    j.timeout_sec = 10;
    auto r = run_now(eng, std::move(j));
    ASSERT_TRUE(r.error == httpp::error_kind::connection);
    ASSERT_TRUE(r.error_message.find("proxy") != std::string::npos);
}

UTEST(async_proxy, keep_alive_works_through_a_tunnel) {
    tls_server origin(testcerts::good_cert, testcerts::good_key, [](const std::string&) { return keep_response("t"); });
    origin.keep_alive = true;
    proxy_test_server proxy;
    async_http eng;
    const std::string ca = write_pem(testcerts::good_cert, "good");
    for (int i = 0; i < 3; ++i) {
        auto j = job_for(origin.port(), true);
        j.tls = true;
        j.ca_file = ca;
        j.proxy_host = "127.0.0.1";
        j.proxy_port = proxy.port();
        ASSERT_EQ(200, run_now(eng, std::move(j)).status);
    }
    ASSERT_EQ(1, proxy.tunnels.load()); // one tunnel, three requests
    ASSERT_EQ(1, origin.handshakes_done.load());
}
