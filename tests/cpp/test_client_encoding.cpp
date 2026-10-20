// The client sends Accept-Encoding and undoes gzip / deflate bodies, on both engines (blocking and async).
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "internal/deflate.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

using httpp::detail::deflate;
using httpp::detail::inflate_format;

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
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }
};

const std::string text = [] {
    std::string s;
    for (int i = 0; i < 400; ++i) s += "line " + std::to_string(i) + " of the body\n";
    return s;
}();

// One server for every case: /gzip, /zlib, /raw, /chain, /plain, /bad, /br, /bomb, /echo-ae, /empty.
httpp::server make_server(std::string* seen_accept_encoding, std::mutex* mu) {
    httpp::server s;
    auto send = [](httpp::response& r, std::string body, const char* coding) {
        r.status = 200;
        r.body = std::move(body);
        r.headers.emplace_back("Content-Encoding", coding);
    };
    s.get("/gzip", [=](const httpp::request& q, httpp::response& r) {
        { std::lock_guard<std::mutex> l(*mu); *seen_accept_encoding = q.header("Accept-Encoding"); }
        send(r, deflate(text.data(), text.size(), inflate_format::gzip, 6), "gzip");
    });
    s.get("/zlib", [=](const httpp::request&, httpp::response& r) { send(r, deflate(text.data(), text.size(), inflate_format::zlib), "deflate"); });
    s.get("/raw", [=](const httpp::request&, httpp::response& r) { send(r, deflate(text.data(), text.size(), inflate_format::raw), "deflate"); });
    s.get("/chain", [=](const httpp::request&, httpp::response& r) {
        // applied in this order: deflate, then gzip -> "Content-Encoding: deflate, gzip"
        const std::string once = deflate(text.data(), text.size(), inflate_format::zlib);
        send(r, deflate(once.data(), once.size(), inflate_format::gzip), "deflate, gzip");
    });
    s.get("/plain", [=](const httpp::request&, httpp::response& r) { r.status = 200; r.body = text; });
    s.get("/bad", [=](const httpp::request&, httpp::response& r) { send(r, std::string("\x1f\x8b\x08\x00 not really gzip"), "gzip"); });
    s.get("/br", [=](const httpp::request&, httpp::response& r) { send(r, "not touched", "br"); });
    s.get("/bomb", [=](const httpp::request&, httpp::response& r) {
        const std::string zeros(1u << 20, '\0');
        send(r, deflate(zeros.data(), zeros.size(), inflate_format::gzip, 9), "gzip");
    });
    s.get("/empty", [=](const httpp::request&, httpp::response& r) { r.status = 204; r.headers.emplace_back("Content-Encoding", "gzip"); });
    return s;
}

struct fixture {
    std::string seen;
    std::mutex mu;
    running_server rs;
    fixture() : rs(make_server(&seen, &mu)) {}
    std::string accept_encoding() { std::lock_guard<std::mutex> l(mu); return seen; }
};

} // namespace

UTEST(client_encoding, blocking_request_decodes_gzip_and_advertises_it) {
    fixture f;
    auto r = httpp::client::request(f.rs.url("/gzip")).run();
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r.body == text);
    ASSERT_TRUE(f.accept_encoding() == "gzip, deflate");
}

UTEST(client_encoding, async_request_decodes_gzip_and_advertises_it) {
    fixture f;
    auto r = httpp::client::request(f.rs.url("/gzip")).run_async().get();
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r.body == text);
    ASSERT_TRUE(f.accept_encoding() == "gzip, deflate");
}

UTEST(client_encoding, session_client_decodes_too) {
    fixture f;
    httpp::client c(f.rs.url(""));
    auto r = c.get("/gzip");
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r.body == text);
    ASSERT_TRUE(f.accept_encoding() == "gzip, deflate");
}

UTEST(client_encoding, deflate_means_zlib_but_raw_is_accepted_and_chains_are_undone) {
    fixture f;
    for (const char* path : {"/zlib", "/raw", "/chain"}) {
        ASSERT_TRUE(httpp::client::request(f.rs.url(path)).run().body == text);
        ASSERT_TRUE(httpp::client::request(f.rs.url(path)).run_async().get().body == text);
    }
}

UTEST(client_encoding, a_plain_body_and_an_unknown_coding_are_left_alone) {
    fixture f;
    ASSERT_TRUE(httpp::client::request(f.rs.url("/plain")).run().body == text);
    auto r = httpp::client::request(f.rs.url("/br")).run();
    ASSERT_TRUE(r.ok());
    ASSERT_TRUE(r.body == "not touched");
    ASSERT_TRUE(httpp::client::request(f.rs.url("/br")).run_async().get().body == "not touched");
}

UTEST(client_encoding, a_caller_supplied_accept_encoding_is_sent_as_is) {
    fixture f;
    auto r = httpp::client::request(f.rs.url("/gzip")).header("Accept-Encoding", "identity").run();
    ASSERT_TRUE(f.accept_encoding() == "identity");
    ASSERT_TRUE(r.body == text);  // this server gzips regardless, and we still decode what we are given
    httpp::client::request(f.rs.url("/gzip")).header("Accept-Encoding", "identity").run_async().get();
    ASSERT_TRUE(f.accept_encoding() == "identity");
}

UTEST(client_encoding, corrupt_data_is_an_error_not_garbage) {
    fixture f;
    auto a = httpp::client::request(f.rs.url("/bad")).run();
    ASSERT_FALSE(a.ok());
    ASSERT_TRUE(a.error == httpp::error_kind::other);
    ASSERT_TRUE(a.body.empty());
    auto b = httpp::client::request(f.rs.url("/bad")).run_async().get();
    ASSERT_TRUE(b.error == httpp::error_kind::other);
    ASSERT_TRUE(b.status == 0);
}

UTEST(client_encoding, max_response_size_limits_the_decoded_size_zip_bomb) {
    fixture f;
    auto a = httpp::client::request(f.rs.url("/bomb")).max_response_size(10000).run();
    ASSERT_TRUE(a.error == httpp::error_kind::too_large);
    ASSERT_EQ(413, a.status);
    auto b = httpp::client::request(f.rs.url("/bomb")).max_response_size(10000).run_async().get();
    ASSERT_TRUE(b.error == httpp::error_kind::too_large);
    ASSERT_EQ(413, b.status);
    // the same body is fine when the limit allows it
    ASSERT_EQ(1u << 20, httpp::client::request(f.rs.url("/bomb")).max_response_size(2u << 20).run().body.size());
}

UTEST(client_encoding, an_empty_body_with_a_content_encoding_is_not_an_error) {
    fixture f;
    auto r = httpp::client::request(f.rs.url("/empty")).run();
    ASSERT_TRUE(r.ok());
    ASSERT_EQ(204, r.status);
    ASSERT_TRUE(httpp::client::request(f.rs.url("/empty")).run_async().get().ok());
}
