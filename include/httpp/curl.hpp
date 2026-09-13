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
class HTTPP_API request {
public:
    explicit request(std::string url);
    ~request();

    request(request&&) noexcept;
    request& operator=(request&&) noexcept;
    request(const request&) = delete;
    request& operator=(const request&) = delete;

    // -X METHOD (GET/POST/PUT/PATCH/DELETE). If never called, defaults to
    // GET, unless data() was called, in which case it defaults to POST —
    // matching curl's own behavior.
    request& method(std::string m);

    // -H "Name: value"
    request& header(std::string name, std::string value);

    // -d "body" (sets Content-Type to application/x-www-form-urlencoded
    // unless content_type() overrides it)
    request& data(std::string body);

    request& content_type(std::string type);

    // -m/--max-time (seconds), -L/--location
    request& timeout(long seconds);
    request& follow_redirects(bool enable = true);

    response run() const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httpp::curl
