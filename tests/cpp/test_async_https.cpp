// HTTPS through the async engine (src/core/async_http.cpp + tls_session.cpp), against a small
// mbedtls TLS server living in this file, so no `openssl` binary and no network are needed.
#include "utest/utest.h"
#include "internal/async_http.hpp"
#include "tls_test_server.hpp"

#ifndef ASIO_STANDALONE
#  define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

using httpp::detail::async_http;
using httpp::detail::async_job;
using asio::ip::tcp;
using testtls::tls_server;
using testtls::ok_response;
using testtls::write_pem;

namespace {

async_job https_job(int port, std::string ca_file) {
    async_job j;
    j.host = "127.0.0.1";
    j.port = port;
    j.tls = true;
    j.ca_file = std::move(ca_file);
    j.timeout_sec = 15;
    return j;
}

std::future<httpp::response> run(async_http& eng, async_job job) {
    auto p = std::make_shared<std::promise<httpp::response>>();
    auto f = p->get_future();
    eng.submit(std::move(job), [p](httpp::response r) { p->set_value(std::move(r)); });
    return f;
}

bool ready(std::future<httpp::response>& f, int seconds = 20) {
    return f.wait_for(std::chrono::seconds(seconds)) == std::future_status::ready;
}

} // namespace

UTEST(async_https, gets_a_body_trusting_a_ca_file) {
    tls_server srv(testcerts::good_cert, testcerts::good_key, [](const std::string&) { return ok_response("secure hello"); });
    async_http eng;
    auto f = run(eng, https_job(srv.port(), write_pem(testcerts::good_cert, "good")));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("secure hello", r.body.c_str());
    ASSERT_FALSE(r.failed());
}

UTEST(async_https, works_over_tls_1_2_too) {
    tls_server srv(testcerts::good_cert, testcerts::good_key, [](const std::string&) { return ok_response("one-two"); }, true);
    async_http eng;
    auto f = run(eng, https_job(srv.port(), write_pem(testcerts::good_cert, "good")));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("one-two", r.body.c_str());
}

UTEST(async_https, sends_host_without_port_only_when_default_and_posts_a_body) {
    std::string seen;
    std::mutex mu;
    tls_server srv(testcerts::good_cert, testcerts::good_key, [&](const std::string& req) {
        std::lock_guard<std::mutex> lk(mu);
        seen = req;
        return ok_response("got " + std::to_string(req.size()));
    });
    async_http eng;
    auto j = https_job(srv.port(), write_pem(testcerts::good_cert, "good"));
    j.method = "POST";
    j.target = "/submit?x=1";
    j.body = "hello=world";
    j.headers = {{"X-Token", "abc"}};
    auto f = run(eng, std::move(j));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(200, r.status);
    std::lock_guard<std::mutex> lk(mu);
    ASSERT_TRUE(seen.rfind("POST /submit?x=1 HTTP/1.1\r\n", 0) == 0);
    ASSERT_TRUE(seen.find("Host: 127.0.0.1:" + std::to_string(srv.port())) != std::string::npos);
    ASSERT_TRUE(seen.find("X-Token: abc") != std::string::npos);
    ASSERT_TRUE(seen.find("Content-Length: 11") != std::string::npos);
    ASSERT_TRUE(seen.size() >= 11 && seen.compare(seen.size() - 11, 11, "hello=world") == 0);
}

UTEST(async_https, large_body_spanning_many_tls_records) {
    std::string big(300000, 'z');
    for (std::size_t i = 0; i < big.size(); i += 997) big[i] = static_cast<char>('a' + i % 26);
    tls_server srv(testcerts::good_cert, testcerts::good_key, [&](const std::string&) { return ok_response(big); });
    async_http eng;
    auto f = run(eng, https_job(srv.port(), write_pem(testcerts::good_cert, "good")));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(r.body == big);
}

UTEST(async_https, large_request_body) {
    std::atomic<std::size_t> got{0};
    tls_server srv(testcerts::good_cert, testcerts::good_key, [&](const std::string& req) {
        got = req.size();
        return ok_response("ok");
    });
    async_http eng;
    auto j = https_job(srv.port(), write_pem(testcerts::good_cert, "good"));
    j.method = "PUT";
    j.body = std::string(200000, 'q');
    auto f = run(eng, std::move(j));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(200, r.status);
    ASSERT_TRUE(got.load() > 200000);
}

UTEST(async_https, many_concurrent_handshakes) {
    tls_server srv(testcerts::good_cert, testcerts::good_key, [](const std::string&) { return ok_response("x"); });
    async_http eng;
    std::string ca = write_pem(testcerts::good_cert, "good");
    std::vector<std::future<httpp::response>> fs;
    for (int i = 0; i < 40; ++i) fs.push_back(run(eng, https_job(srv.port(), ca)));
    int good = 0;
    for (auto& f : fs) {
        ASSERT_TRUE(ready(f, 40));
        auto r = f.get();
        if (r.status == 200 && r.body == "x") ++good;
    }
    ASSERT_EQ(40, good);
}

UTEST(async_https, untrusted_certificate_is_a_tls_error) {
    tls_server srv(testcerts::good_cert, testcerts::good_key, [](const std::string&) { return ok_response("never"); });
    async_http eng;
    auto f = run(eng, https_job(srv.port(), write_pem(testcerts::other_cert, "other"))); // trusts a different cert
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(0, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::tls);
    ASSERT_TRUE(r.body.empty());
}

UTEST(async_https, system_roots_do_not_trust_a_self_signed_certificate) {
    tls_server srv(testcerts::good_cert, testcerts::good_key, [](const std::string&) { return ok_response("never"); });
    async_http eng;
    auto f = run(eng, https_job(srv.port(), ""));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_TRUE(r.error == httpp::error_kind::tls);
}

UTEST(async_https, host_name_mismatch_is_a_tls_error) {
    // The server presents a certificate for other.example and the client trusts exactly that
    // certificate, but connects to 127.0.0.1: only the name check can reject it.
    tls_server srv(testcerts::other_cert, testcerts::other_key, [](const std::string&) { return ok_response("never"); });
    async_http eng;
    auto f = run(eng, https_job(srv.port(), write_pem(testcerts::other_cert, "other")));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_TRUE(r.error == httpp::error_kind::tls);
}

UTEST(async_https, verify_false_accepts_anything) {
    tls_server srv(testcerts::other_cert, testcerts::other_key, [](const std::string&) { return ok_response("unverified"); });
    async_http eng;
    auto j = https_job(srv.port(), "");
    j.verify = false;
    auto f = run(eng, std::move(j));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("unverified", r.body.c_str());
}

UTEST(async_https, missing_ca_file_is_a_tls_error_not_a_crash) {
    async_http eng;
    auto f = run(eng, https_job(1, "/no/such/ca.pem"));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_TRUE(r.error == httpp::error_kind::tls);
}

UTEST(async_https, plain_http_server_on_the_https_port_is_a_tls_error) {
    asio::io_context io;
    tcp::acceptor acc(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    std::thread th([&] {
        tcp::socket s(io);
        std::error_code ec;
        acc.accept(s, ec);
        if (ec) return;
        asio::write(s, asio::buffer(std::string("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")), ec);
        s.shutdown(tcp::socket::shutdown_both, ec);
    });
    async_http eng;
    auto f = run(eng, https_job(acc.local_endpoint().port(), write_pem(testcerts::good_cert, "good")));
    bool done = ready(f);
    std::error_code ec;
    acc.close(ec);
    th.join();
    ASSERT_TRUE(done);
    auto r = f.get();
    ASSERT_EQ(0, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::tls);
}

UTEST(async_https, server_that_stops_talking_times_out_during_the_handshake) {
    asio::io_context io;
    tcp::acceptor acc(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    tcp::socket held(io);
    std::thread th([&] { std::error_code ec; acc.accept(held, ec); }); // accepts, then says nothing
    async_http eng;
    auto j = https_job(acc.local_endpoint().port(), write_pem(testcerts::good_cert, "good"));
    j.timeout_sec = 1;
    auto f = run(eng, std::move(j));
    bool done = ready(f);
    th.join();
    ASSERT_TRUE(done);
    auto r = f.get();
    ASSERT_TRUE(r.error == httpp::error_kind::timeout);
}

UTEST(async_https, connection_dropped_without_close_notify_after_a_complete_response_is_fine) {
    tls_server srv(testcerts::good_cert, testcerts::good_key, [](const std::string&) { return ok_response("complete"); });
    srv.close_notify = false;
    async_http eng;
    auto f = run(eng, https_job(srv.port(), write_pem(testcerts::good_cert, "good")));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("complete", r.body.c_str());
}

UTEST(async_https, truncated_response_over_tls_is_a_connection_error) {
    tls_server srv(testcerts::good_cert, testcerts::good_key, [](const std::string&) {
        return std::string("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
    });
    async_http eng;
    auto f = run(eng, https_job(srv.port(), write_pem(testcerts::good_cert, "good")));
    ASSERT_TRUE(ready(f));
    auto r = f.get();
    ASSERT_EQ(0, r.status);
    ASSERT_TRUE(r.error == httpp::error_kind::connection);
}
