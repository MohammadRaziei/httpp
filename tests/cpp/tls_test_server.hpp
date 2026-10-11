#pragma once
// A tiny TLS server (mbedtls, blocking, one thread per connection) for the engine tests, so they need
// no `openssl` binary. Shared by test_async_https.cpp and test_async_pool_proxy.cpp.

#include "internal/async_http.hpp"
#include "test_certs.hpp"

#ifndef ASIO_STANDALONE
#  define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace testtls {

using asio::ip::tcp;

// Serves HTTP/1.1 over TLS; one thread per connection; `handler(request_text) -> response_text`.
struct tls_server {
    using handler_fn = std::function<std::string(const std::string&)>;

    asio::io_context io;
    tcp::acceptor acc{io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
    mbedtls_ssl_config conf;
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
    handler_fn handler;
    std::thread th;
    std::mutex mu;
    std::vector<std::thread> workers;
    std::atomic<bool> stopping{false};
    std::atomic<int> handshakes_done{0};
    std::atomic<int> requests_served{0};
    bool close_notify = true;
    bool keep_alive = false; // serve several requests per connection (else one, then close)

    int port() { return acc.local_endpoint().port(); }

    tls_server(const char* cert, const char* key, handler_fn h, bool tls12_only = false) : handler(std::move(h)) {
        psa_crypto_init();
        mbedtls_ssl_config_init(&conf); mbedtls_x509_crt_init(&crt); mbedtls_pk_init(&pk);
        mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char*>(cert), std::strlen(cert) + 1);
        mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(key), std::strlen(key) + 1, nullptr, 0);
        mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_own_cert(&conf, &crt, &pk);
        if (tls12_only) mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
        th = std::thread([this] { accept_loop(); });
    }
    ~tls_server() {
        stopping = true;
        { // closing the acceptor does not wake a blocked accept() on every OS: knock on the door instead
            tcp::socket knock(io);
            std::error_code ec;
            knock.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port())), ec);
        }
        if (th.joinable()) th.join();
        for (auto& w : workers) w.join();
        mbedtls_ssl_config_free(&conf); mbedtls_x509_crt_free(&crt); mbedtls_pk_free(&pk);
    }

    void accept_loop() {
        while (!stopping) {
            auto s = std::make_shared<tcp::socket>(io);
            std::error_code ec;
            acc.accept(*s, ec);
            if (ec) return;
            std::lock_guard<std::mutex> lk(mu);
            workers.emplace_back([this, s] { serve(s); });
        }
    }

    static int send_cb(void* p, const unsigned char* b, std::size_t n) {
        std::error_code ec;
        std::size_t w = asio::write(*static_cast<tcp::socket*>(p), asio::buffer(b, n), ec);
        return ec ? MBEDTLS_ERR_SSL_CONN_EOF : static_cast<int>(w);
    }
    static int recv_cb(void* p, unsigned char* b, std::size_t n) {
        std::error_code ec;
        std::size_t r = static_cast<tcp::socket*>(p)->read_some(asio::buffer(b, n), ec);
        return ec ? MBEDTLS_ERR_SSL_CONN_EOF : static_cast<int>(r);
    }

    void serve(std::shared_ptr<tcp::socket> s) {
        mbedtls_ssl_context ssl;
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_setup(&ssl, &conf);
        mbedtls_ssl_set_bio(&ssl, s.get(), send_cb, recv_cb, nullptr);
        if (mbedtls_ssl_handshake(&ssl) == 0) {
            ++handshakes_done;
            bool open = true;
            while (open) {
                std::string req;
                unsigned char buf[4096];
                std::size_t need = std::string::npos;
                for (;;) {
                    int r = mbedtls_ssl_read(&ssl, buf, sizeof buf);
                    if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
                    if (r <= 0) { open = false; break; }
                    req.append(reinterpret_cast<char*>(buf), static_cast<std::size_t>(r));
                    auto end = req.find("\r\n\r\n");
                    if (end == std::string::npos) continue;
                    if (need == std::string::npos) {
                        need = end + 4;
                        auto cl = req.find("Content-Length: ");
                        if (cl != std::string::npos && cl < end) need += std::stoul(req.substr(cl + 16));
                    }
                    if (req.size() >= need) break;
                }
                if (req.empty()) break;
                ++requests_served;
                std::string resp = handler(req);
                for (std::size_t off = 0; off < resp.size();) {
                    int w = mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(resp.data()) + off, resp.size() - off);
                    if (w <= 0) { open = false; break; }
                    off += static_cast<std::size_t>(w);
                }
                if (!keep_alive) break;
            }
            if (close_notify) mbedtls_ssl_close_notify(&ssl);
        }
        std::error_code ec;
        s->shutdown(tcp::socket::shutdown_both, ec);
        mbedtls_ssl_free(&ssl);
    }
};

inline std::string ok_response(const std::string& body) {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

// Same, but leaves the connection open (no Connection: close), for keep-alive tests.
inline std::string keep_response(const std::string& body) {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

inline std::string write_pem(const char* pem, const char* name) {
    std::string path = (std::filesystem::temp_directory_path() / (std::string("httpp_test_") + name + ".pem")).string();
    std::ofstream(path, std::ios::binary) << pem;
    return path;
}

} // namespace testtls
