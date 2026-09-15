#pragma once

#include "httpp/export.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace httpp {

struct response {
    int status = 0;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    bool ok() const { return status >= 200 && status < 300; }

    // HTTP header names are case-insensitive (RFC 7230); this looks up
    // the first matching header and returns "" if it isn't present.
    std::string header(const std::string& name) const {
        auto ci_equal = [](const std::string& a, const std::string& b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
                       return std::tolower(x) == std::tolower(y);
                   });
        };
        for (const auto& [key, value] : headers) {
            if (ci_equal(key, name)) return value;
        }
        return "";
    }
};

// Public API exposes no third-party types (no httplib.h here at all).
// The real implementation (backed by vendored cpp-httplib + mbedtls for
// HTTPS) lives in src/core/client.cpp and is hidden behind this pointer, so
// neither httplib.h nor mbedtls ever leak into consumers of this header.
class client {
public:
    // use_ssl selects httplib's SSLClient (mbedtls-backed) vs. its plain
    // Client internally; both http:// and https:// URLs work through
    // fetch()/client::request without the caller ever thinking about this.
    explicit HTTPP_API client(const std::string& host, int port, bool use_ssl = false);
    HTTPP_API ~client();

    HTTPP_API client(client&&) noexcept;
    HTTPP_API client& operator=(client&&) noexcept;
    client(const client&) = delete;
    client& operator=(const client&) = delete;

    HTTPP_API response get(const std::string& path);
    HTTPP_API response get(const std::string& path, const std::vector<std::pair<std::string, std::string>>& headers);

    // Parse `full_url` (via httpp::url) and GET it in one call — handles
    // http:// and https:// alike. No manual URL parsing needed by callers
    // (e.g. the CLI's `download` command).
    static HTTPP_API response fetch(const std::string& full_url);

    // A small, curl-flavored fluent request builder for the less common
    // cases (custom method, headers, a body) that get()/fetch() don't
    // cover — the common cases only (-X/-H/-d equivalents), not a full
    // HTTP-client-options kitchen sink. Also what httpp/curl_compat.h's
    // `curl_easy_*` C API is built on (see src/core/curl_compat.cpp) — one
    // implementation of "run an HTTP request" in httpp, not two.
    //
    // Nested under `client` (not a free `httpp::request`) because
    // `httpp::request` is already the incoming-request struct passed to
    // server route handlers (see httpp/server.hpp) — naming this
    // `client::request` avoids that collision and reads naturally next to
    // the class it's built on: httpp::client::request(url)....run()
    class request {
    public:
        explicit HTTPP_API request(std::string url);
        HTTPP_API ~request();

        HTTPP_API request(request&&) noexcept;
        HTTPP_API request& operator=(request&&) noexcept;
        request(const request&) = delete;
        request& operator=(const request&) = delete;

        // -X METHOD (GET/POST/PUT/PATCH/DELETE). If never called, defaults
        // to GET, unless data() was called, in which case it defaults to
        // POST — matching curl's own behavior.
        HTTPP_API request& method(std::string m);

        // -H "Name: value"
        HTTPP_API request& header(std::string name, std::string value);

        // -d "body" (sets Content-Type to
        // application/x-www-form-urlencoded unless content_type() overrides it)
        HTTPP_API request& data(std::string body);

        HTTPP_API request& content_type(std::string type);

        // -m/--max-time (seconds), -L/--location
        HTTPP_API request& timeout(long seconds);
        HTTPP_API request& follow_redirects(bool enable = true);

        HTTPP_API response run() const;

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httpp
