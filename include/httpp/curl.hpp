#pragma once

#include "httpp/export.hpp"
#include "httpp/client.hpp" // reuses httpp::response

#include <memory>
#include <string>

namespace httpp::curl {

// A small, curl-flavored fluent request builder — the common cases only
// (-X method, -H headers, -d data), NOT a libcurl API/ABI compatible shim
// (that's a much bigger, different project; see httpp::client for the
// plain request API this is built on). Backed by the same vendored
// cpp-httplib as the rest of httpp, hidden behind this pointer (see
// src/core/curl.cpp) and never exposed in this public header.
class request {
public:
    explicit HTTPP_API request(std::string url);
    HTTPP_API ~request();

    HTTPP_API request(request&&) noexcept;
    HTTPP_API request& operator=(request&&) noexcept;
    request(const request&) = delete;
    request& operator=(const request&) = delete;

    // -X METHOD (GET/POST/PUT/PATCH/DELETE). If never called, defaults to
    // GET, unless data() was called, in which case it defaults to POST —
    // matching curl's own behavior.
    HTTPP_API request& method(std::string m);

    // -H "Name: value"
    HTTPP_API request& header(std::string name, std::string value);

    // -d "body" (sets Content-Type to application/x-www-form-urlencoded
    // unless content_type() overrides it)
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

} // namespace httpp::curl
