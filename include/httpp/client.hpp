#pragma once

#include "httpp/export.hpp"

#include <memory>
#include <string>

namespace httpp {

struct HTTPP_API response {
    int status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

// Public API exposes no third-party types (no httplib.h here at all).
// The real implementation (backed by vendored cpp-httplib) lives in
// src/client.cpp and is hidden behind this pointer, so httplib.h never
// leaks into consumers of <httpp/client.hpp> and stays compiled into
// the httpp .so/.a only.
class HTTPP_API client {
public:
    explicit client(const std::string& host, int port);
    ~client();

    client(client&&) noexcept;
    client& operator=(client&&) noexcept;
    client(const client&) = delete;
    client& operator=(const client&) = delete;

    response get(const std::string& path);

    // Parse `full_url` (via httpp::url) and GET it in one call, so callers
    // (like the CLI's `download` command) never have to parse a URL
    // themselves. Throws std::invalid_argument for an unsupported/invalid
    // URL (only http/https are handled).
    static response fetch(const std::string& full_url);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httpp
