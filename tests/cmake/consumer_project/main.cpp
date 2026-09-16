// End-to-end smoke test for the *installed* httpp CMake package: this file
// only ever includes <httpp.h> (found via find_package(httpp) in
// CMakeLists.txt here) and links against httpp::httpp_core — nothing else.
// It exercises every major piece of the public API to prove the installed
// package is actually complete and usable, not just able to parse a URL.
#include <httpp.h>
#include <httpp/curl_compat.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::cout << "[OK]   " << what << "\n";
    } else {
        std::cout << "[FAIL] " << what << "\n";
        ++failures;
    }
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t write_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

int main() {
    // 1) httpp::url
    httpp::url u = httpp::url::parse("http://example.com:8080/hello?x=1");
    check(u.valid() && u.host() == "example.com" && u.port() == 8080 && u.path() == "/hello",
          "httpp::url parses scheme/host/port/path/query");

    // 2) httpp::server + httpp::client, live over a real socket
    httpp::server srv;
    srv.get("/hello", [](const httpp::request&, httpp::response& res) {
        res.status = 200;
        res.body = "world";
    });
    const int port = srv.bind_to_any_port("127.0.0.1");
    std::thread server_thread([&srv] { srv.listen_after_bind(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    {
        httpp::client cli("127.0.0.1", port);
        httpp::response res = cli.get("/hello");
        check(res.ok() && res.body == "world", "httpp::client::get talks to httpp::server");
    }

    {
        httpp::response res = httpp::client::fetch(base + "/hello");
        check(res.ok() && res.body == "world", "httpp::client::fetch parses a full URL and gets it");
    }

    // 3) httpp::client::request (fluent builder)
    {
        httpp::response res = httpp::client::request(base + "/hello")
                                   .method("GET")
                                   .header("X-Test", "abc")
                                   .run();
        check(res.ok() && res.body == "world", "httpp::client::request GETs with a custom header");
    }
    {
        httpp::response res = httpp::client::request(base + "/missing").method("DELETE").run();
        check(res.status == 404, "httpp::client::request sends a custom DELETE method");
    }

    // 4) httpp/curl_compat.h (libcurl-style C API)
    {
        CURL* curl = curl_easy_init();
        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, (base + "/hello").c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        CURLcode rc = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        check(rc == CURLE_OK && http_code == 200 && body == "world",
              "curl_easy_perform (compat layer) GETs successfully");
        curl_easy_cleanup(curl);
    }

    // 5) httpp::download (fluent builder + progress bar plumbing)
    {
        const std::string dest = "consumer_download_output.txt";
        std::remove(dest.c_str());
        httpp::download_result res = httpp::download(base + "/hello")
                                          .output(dest)
                                          .disable_progress()
                                          .run();
        check(res.ok && read_file(dest) == "world", "httpp::download downloads to a file");
        std::remove(dest.c_str());
    }

    // 6) httpp::progress::range (trange-like)
    {
        std::size_t count = 0;
        for (auto i : httpp::progress::range(5, "consumer-check")) {
            (void)i;
            ++count;
        }
        check(count == 5, "httpp::progress::range iterates the expected number of times");
    }

    srv.stop();
    server_thread.join();

    if (failures == 0) {
        std::cout << "httpp found and linked OK: all checks passed\n";
        return 0;
    }
    std::cout << failures << " check(s) failed\n";
    return 1;
}
