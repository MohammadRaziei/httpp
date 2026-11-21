// ADR-0105: request::clone(), server::clone() and download::header(name, value).
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/download.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

using testutil::local_server;

// "METHOD path?query|content-type|authorization|body|X-Default|cookie"
void echo_route(httpp::server& s) {
    auto handler = [](const httpp::server_request& req, httpp::response& res) {
        std::string query;
        for (const auto& [k, v] : req.query) query += (query.empty() ? "" : "&") + k + "=" + v;
        res.body = req.method + " " + req.path + "?" + query + "|" + req.header("Content-Type") + "|" + req.header("Authorization") +
                   "|" + req.body + "|" + req.header("X-Default") + "|" + req.header("Cookie");
    };
    for (const char* m : {"GET", "POST", "PUT", "PATCH", "DELETE"}) s.route(m, "/echo", handler);
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const char* const hello_sum = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";

} // namespace

static_assert(!std::is_copy_constructible<httpp::request>::value, "request stays move-only; clone() is the explicit copy");
static_assert(std::is_copy_constructible<httpp::server>::value, "server stays a handle; clone() is the independent copy");

// --- request::clone ---

UTEST(httpp_request_clone, a_clone_is_independent_of_the_original) {
    local_server ls(echo_route);
    auto base = httpp::request(httpp::url(ls.url("/echo"))).bearer("base").param("a", "1");
    auto other = base.clone();
    other.param("b", "2").bearer("other").method("PUT").data("x").content_type("text/plain");

    ASSERT_STREQ("GET /echo?a=1||Bearer base|||", base.run().body.c_str()); // the original did not change
    ASSERT_STREQ("PUT /echo?a=1&b=2|text/plain|Bearer other|x||", other.run().body.c_str());
    base.param("c", "3");
    ASSERT_STREQ("PUT /echo?a=1&b=2|text/plain|Bearer other|x||", other.run().body.c_str()); // and back
}

UTEST(httpp_request_clone, a_request_clone_carries_every_field) {
    local_server ls(echo_route);
    const std::string file = "clone_every_field.bin";
    std::remove(file.c_str());
    auto base = httpp::request(httpp::url(ls.url("/echo")));
    base.method("PATCH").json("{\"k\":1}").header("X-Default", "d").param("q", "v").cookie("sid", "7").timeout(9).max_redirects(3)
        .max_time(30).retry(2).retry_delay(1).verify(false).auto_compression(false).max_response_size(1000);
    base.save_to(file).create_dirs(true).overwrite(false).include_head(false);
    std::string streamed;
    base.on_body([&](std::string_view piece) { streamed.append(piece); return true; });
    bool head_seen = false;
    base.on_head([&](const httpp::response&) { head_seen = true; return true; });

    auto copy = base.clone();
    ASSERT_EQ(9, copy.timeout());
    ASSERT_EQ(3, copy.max_redirects());
    ASSERT_EQ(30, copy.max_time());
    ASSERT_EQ(2, copy.retry());
    ASSERT_EQ(1, copy.retry_delay());
    ASSERT_FALSE(copy.verify());
    ASSERT_FALSE(copy.auto_compression());

    const auto res = copy.run();
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ(file.c_str(), res.saved_as.c_str());
    const std::string expected = "PATCH /echo?q=v|application/json||{\"k\":1}|d|sid=7";
    ASSERT_STREQ(expected.c_str(), read_file(file).c_str()); // method, body, content type, header, param, cookie, save_to
    ASSERT_STREQ(expected.c_str(), streamed.c_str());         // on_body
    ASSERT_TRUE(head_seen);                                    // on_head

    const auto again = copy.run(); // overwrite(false) was copied too: the file is there now
    ASSERT_TRUE(again.error == httpp::error_kind::file);
    std::remove(file.c_str());
}

// --- server::clone ---

UTEST(httpp_server_clone, a_clone_has_the_routes_hooks_and_handlers_and_then_goes_its_own_way) {
    httpp::server base;
    base.get("/hello", [](const httpp::server_request&, httpp::response& r) { r.body = "hello"; });
    base.after_request([](const httpp::server_request&, httpp::response& r) { r.headers.emplace_back("X-After", "yes"); });
    base.use([](const httpp::server_request& q, httpp::response& r) {
        if (q.path != "/blocked") return false;
        r.status = 403;
        r.body = "blocked";
        return true;
    });
    base.error_handler([](const httpp::server_request&, httpp::response& r) {
        if (r.status == 404) r.body = "custom 404";
    });

    httpp::server copy = base.clone();
    ASSERT_FALSE(copy.same_as(base));
    copy.get("/only-copy", [](const httpp::server_request&, httpp::response& r) { r.body = "copy"; });
    base.get("/only-base", [](const httpp::server_request&, httpp::response& r) { r.body = "base"; });

    local_server a([&](httpp::server& s) { s = base; });
    local_server b([&](httpp::server& s) { s = copy; });

    const auto hello = httpp::client(httpp::url(b.url())).get("/hello");
    ASSERT_EQ(200, hello.status);
    ASSERT_STREQ("hello", hello.body.c_str());
    ASSERT_STREQ("yes", hello.header("X-After").c_str()); // the application's after hook
    ASSERT_EQ(403, httpp::client(httpp::url(b.url())).get("/blocked").status); // middleware
    const auto missing = httpp::client(httpp::url(b.url())).get("/nothing");
    ASSERT_EQ(404, missing.status);
    ASSERT_STREQ("custom 404", missing.body.c_str()); // the error handler

    ASSERT_STREQ("copy", httpp::client(httpp::url(b.url())).get("/only-copy").body.c_str());
    ASSERT_EQ(404, httpp::client(httpp::url(a.url())).get("/only-copy").status); // a route added to the clone is not on the original
    ASSERT_STREQ("base", httpp::client(httpp::url(a.url())).get("/only-base").body.c_str());
    ASSERT_EQ(404, httpp::client(httpp::url(b.url())).get("/only-base").status); // and the other way round
}

UTEST(httpp_server_clone, parametric_routes_and_the_engine_settings_are_copied) {
    httpp::server base;
    base.get("/users/{id}", [](const httpp::server_request& q, httpp::response& r) { r.body = "user " + (q.path_params.empty() ? std::string() : q.path_params[0].second); });
    base.max_body(4).io_threads(2).keep_alive_max(7);
    httpp::server copy = base.clone();

    local_server ls([&](httpp::server& s) { s = copy; });
    httpp::client c(httpp::url(ls.url()));
    ASSERT_STREQ("user 42", c.get("/users/42").body.c_str());
    ASSERT_EQ(413, c.post("/users/42", "too long a body").status); // max_body came along
}

UTEST(httpp_server_clone, cloning_a_listening_server_throws_and_a_bound_one_is_fine) {
    local_server ls(echo_route);
    bool threw = false;
    try {
        ls.srv.clone();
    } catch (const std::logic_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw);

    httpp::server bound;
    bound.get("/x", [](const httpp::server_request&, httpp::response& r) { r.body = "x"; });
    ASSERT_GE(bound.bind_to_any_port("127.0.0.1"), 0);
    httpp::server copy = bound.clone(); // bound, not listening
    ASSERT_STREQ("", copy.url().c_str()); // the clone has no socket
}

// --- download::header ---

UTEST(httpp_download_header, a_header_reaches_the_file_and_a_second_call_replaces_it) {
    std::mutex mu;
    std::string seen;
    local_server ls([&](httpp::server& s) {
        s.get("/f", [&](const httpp::server_request& q, httpp::response& r) {
            std::lock_guard<std::mutex> lock(mu);
            seen = q.header("X-Api-Key") + "|" + q.header("Accept-Encoding");
            r.body = "hello";
        });
    });
    const std::string path = "download_header.bin";
    std::remove(path.c_str());
    const auto res = httpp::download(httpp::url(ls.url("/f")), path).disable_progress().header("X-Api-Key", "one").header("x-api-key", "two").run();
    ASSERT_TRUE(res.ok);
    ASSERT_STREQ("hello", read_file(path).c_str());
    std::remove(path.c_str());
    std::lock_guard<std::mutex> lock(mu);
    ASSERT_STREQ("two|identity", seen.c_str());
}

UTEST(httpp_download_header, range_and_accept_encoding_belong_to_the_download) {
    httpp::download d{httpp::url("http://127.0.0.1:1/f")};
    for (const char* name : {"Range", "range", "Accept-Encoding", "ACCEPT-ENCODING"}) {
        bool threw = false;
        try {
            d.header(name, "x");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        ASSERT_TRUE(threw);
    }
}

UTEST(httpp_download_header, it_is_sent_with_the_probe_and_to_a_checksum_file_only_on_the_same_server) {
    std::mutex mu;
    std::string probe_key, same_key = "unset", other_key = "unset";
    local_server other([&](httpp::server& s) {
        s.get("/sum", [&](const httpp::server_request& q, httpp::response& r) {
            std::lock_guard<std::mutex> lock(mu);
            other_key = q.header("X-Api-Key");
            r.body = std::string(hello_sum) + "\n";
        });
    });
    local_server same([&](httpp::server& s) {
        s.get("/f", [&](const httpp::server_request& q, httpp::response& r) { // a HEAD runs the GET route
            if (q.method == "HEAD") {
                std::lock_guard<std::mutex> lock(mu);
                probe_key = q.header("X-Api-Key");
            }
            r.body = "hello";
        });
        s.get("/sum", [&](const httpp::server_request& q, httpp::response& r) {
            std::lock_guard<std::mutex> lock(mu);
            same_key = q.header("X-Api-Key");
            r.body = std::string(hello_sum) + "\n";
        });
    });
    const std::string path = "download_header_sum.bin";
    std::remove(path.c_str());
    std::remove((path + ".part").c_str());

    auto res = httpp::download(httpp::url(same.url("/f")), path).disable_progress().resume().header("X-Api-Key", "k")
                   .sha256_url(httpp::url(same.url("/sum"))).run();
    ASSERT_TRUE(res.ok);
    std::remove(path.c_str());
    res = httpp::download(httpp::url(same.url("/f")), path).disable_progress().header("X-Api-Key", "k")
              .sha256_url(httpp::url(other.url("/sum"))).run();
    ASSERT_TRUE(res.ok);
    std::remove(path.c_str());

    std::lock_guard<std::mutex> lock(mu);
    ASSERT_STREQ("k", probe_key.c_str()); // the size probe of resume()
    ASSERT_STREQ("k", same_key.c_str());
    ASSERT_STREQ("", other_key.c_str()); // another server: nothing
}
