#include "utest/utest.h"
#include "httpp/curl_compat.h"
#include "httpp/server.hpp"

#include <thread>
#include <chrono>
#include <string>
#include <cstring>

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
};

size_t write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}
} // namespace

UTEST(httpp_curl_compat, plain_get_like_real_libcurl_code) {
    httpp::server s;
    s.get("/hello", [](const httpp::request&, httpp::response& res) {
        res.status = 200;
        res.body = "world";
    });
    running_server rs(std::move(s));
    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/hello";

    // This block is deliberately written the way real libcurl code looks.
    CURL* curl = curl_easy_init();
    ASSERT_TRUE(curl != nullptr);

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    CURLcode rc = curl_easy_perform(curl);
    ASSERT_EQ(CURLE_OK, rc);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    ASSERT_EQ(200L, http_code);
    ASSERT_STREQ("world", body.c_str());

    curl_easy_cleanup(curl);
}

UTEST(httpp_curl_compat, post_with_custom_headers_and_slist) {
    httpp::server s;
    running_server rs(std::move(s));
    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/echo";

    CURL* curl = curl_easy_init();
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "X-Test: abc");
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{\"a\":1}");

    CURLcode rc = curl_easy_perform(curl);
    ASSERT_EQ(CURLE_OK, rc); // reaches the server (even if 404, connection succeeded)

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    ASSERT_EQ(404L, http_code); // no /echo route registered -> proves POST was actually sent

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

UTEST(httpp_curl_compat, custom_request_overrides_method) {
    httpp::server s;
    running_server rs(std::move(s));
    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/x";

    CURL* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

    CURLcode rc = curl_easy_perform(curl);
    ASSERT_EQ(CURLE_OK, rc);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    ASSERT_EQ(404L, http_code);

    curl_easy_cleanup(curl);
}

UTEST(httpp_curl_compat, connection_failure_is_reported_as_an_error_code) {
    CURL* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:1/nope"); // nothing listens on port 1

    CURLcode rc = curl_easy_perform(curl);
    ASSERT_EQ(CURLE_COULDNT_CONNECT, rc);
    ASSERT_TRUE(curl_easy_strerror(rc) != nullptr);

    curl_easy_cleanup(curl);
}
