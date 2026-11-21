// ADR-0093: download takes the network settings of client and request from configurable.
#include "utest/utest.h"
#include "httpp/download.hpp"
#include "local_server.hpp"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string temp_path(const char* name) { return std::string("download_settings_") + name + ".bin"; }

void forget(const std::string& p) {
    std::remove(p.c_str());
    std::remove((p + ".part").c_str());
}

// "/echo" answers with what the request said about itself: "Authorization|Accept-Encoding".
void echo_route(httpp::server& s) {
    s.get("/echo", [](const httpp::server_request& q, httpp::response& r) {
        r.body = q.header("Authorization") + "|" + q.header("Accept-Encoding");
    });
}

// The sha256 of "hello".
const char* const hello_sum = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";

} // namespace

static_assert(!std::is_copy_constructible<httpp::download>::value, "download is a one-job builder, like request");
static_assert(std::is_move_constructible<httpp::download>::value, "download is a one-job builder, like request");

UTEST(download_settings, the_defaults_are_the_ones_of_a_download) {
    httpp::download d(httpp::url("http://127.0.0.1:1/x"), "unused.bin");
    ASSERT_EQ(20, d.max_redirects()); // redirects are followed here, unlike in request
    ASSERT_EQ(0, d.timeout());
    ASSERT_TRUE(d.verify());
}

UTEST(download_settings, the_rules_are_the_ones_of_the_other_classes) {
    httpp::download d(httpp::url("http://127.0.0.1:1/x"), "unused.bin");
    ASSERT_EQ(0, d.timeout(-5).timeout());
    ASSERT_EQ(-1, d.max_redirects(-1).max_redirects());
    ASSERT_EQ(0, d.max_redirects(0).max_redirects());
    bool threw = false;
    try { d.max_redirects(-2); } catch (const std::invalid_argument&) { threw = true; }
    ASSERT_TRUE(threw);
    threw = false;
    try { d.max_time(-1); } catch (const std::invalid_argument&) { threw = true; }
    ASSERT_TRUE(threw);
    threw = false;
    try { d.proxy(httpp::url("host:3128")); } catch (const std::invalid_argument&) { threw = true; }
    ASSERT_TRUE(threw);
    ASSERT_STREQ("x.pem", d.ca_file("x.pem").ca_file().str().c_str());
}

UTEST(download_settings, a_chain_on_a_temporary_is_a_download) {
    auto d = httpp::download(httpp::url("http://127.0.0.1:1/x")).output("unused.bin").timeout(5).bearer("t").disable_progress().max_redirects(3);
    static_assert(std::is_same<decltype(d), httpp::download>::value, "the chain on a temporary yields a download, not a reference");
    ASSERT_EQ(5, d.timeout());
    ASSERT_EQ(3, d.max_redirects());
}

UTEST(download_settings, bearer_and_basic_auth_reach_the_server) {
    testutil::local_server srv(echo_route);
    const auto path = temp_path("bearer");
    forget(path);
    auto res = httpp::download(httpp::url(srv.url("/echo")), path).disable_progress().bearer("tok").run();
    ASSERT_TRUE(res.ok);
    ASSERT_STREQ("Bearer tok|identity", read_file(path).c_str());
    forget(path);

    res = httpp::download(httpp::url(srv.url("/echo")), path).disable_progress().basic_auth("u", "p").run();
    ASSERT_TRUE(res.ok);
    ASSERT_STREQ("Basic dTpw|identity", read_file(path).c_str());
    forget(path);
}

namespace {
// Detection of a call that does not compile: the deleted names must not be callable on a download.
template <class D, class = void> struct has_retry : std::false_type {};
template <class D> struct has_retry<D, decltype(void(std::declval<D&>().retry(1)))> : std::true_type {};
template <class D, class = void> struct has_auto_compression : std::false_type {};
template <class D> struct has_auto_compression<D, decltype(void(std::declval<D&>().auto_compression(true)))> : std::true_type {};
template <class D, class = void> struct has_timeout : std::false_type {};
template <class D> struct has_timeout<D, decltype(void(std::declval<D&>().timeout(1)))> : std::true_type {};
} // namespace

static_assert(has_retry<httpp::request>::value && has_auto_compression<httpp::request>::value, "the detection works on a request");
static_assert(has_timeout<httpp::download>::value, "the shared settings are there");
static_assert(has_retry<httpp::download>::value, "retry is for a download since ADR-0097");
static_assert(!has_auto_compression<httpp::download>::value, "auto_compression is not for a download (ADR-0093)");

UTEST(download_settings, a_download_always_asks_for_identity_coding) {
    testutil::local_server srv(echo_route);
    const auto path = temp_path("identity");
    forget(path);
    auto res = httpp::download(httpp::url(srv.url("/echo")), path).disable_progress().run();
    ASSERT_TRUE(res.ok);
    ASSERT_STREQ("|identity", read_file(path).c_str());
    forget(path);
}

UTEST(download_settings, the_credentials_go_to_a_checksum_file_on_the_same_server_only) {
    std::mutex mu;
    std::string same_auth, other_auth;
    testutil::local_server other([&](httpp::server& s) {
        s.get("/sum", [&](const httpp::server_request& q, httpp::response& r) {
            std::lock_guard<std::mutex> lock(mu);
            other_auth = q.header("Authorization");
            r.body = std::string(hello_sum) + "\n";
        });
    });
    testutil::local_server same([&](httpp::server& s) {
        s.get("/f", [](const httpp::server_request&, httpp::response& r) { r.body = "hello"; });
        s.get("/sum", [&](const httpp::server_request& q, httpp::response& r) {
            std::lock_guard<std::mutex> lock(mu);
            same_auth = q.header("Authorization");
            r.body = std::string(hello_sum) + "\n";
        });
    });
    const auto path = temp_path("sumorigin");

    forget(path);
    auto res = httpp::download(httpp::url(same.url("/f")), path).disable_progress().bearer("tok").sha256_url(httpp::url(same.url("/sum"))).run();
    ASSERT_TRUE(res.ok);
    ASSERT_STREQ(hello_sum, res.sha256.c_str());
    forget(path);

    res = httpp::download(httpp::url(same.url("/f")), path).disable_progress().bearer("tok").sha256_url(httpp::url(other.url("/sum"))).run();
    ASSERT_TRUE(res.ok);
    forget(path);

    std::lock_guard<std::mutex> lock(mu);
    ASSERT_STREQ("Bearer tok", same_auth.c_str());
    ASSERT_STREQ("", other_auth.c_str()); // another server (another port): no credentials
}
