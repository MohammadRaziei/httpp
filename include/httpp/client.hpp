#pragma once

#include "httpp/export.hpp"

#include <memory>
#include <string>

namespace httpp {

struct HTTPP_API Response {
    int status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

// Public API exposes no third-party types (no httplib.h here at all).
// The real implementation (backed by vendored cpp-httplib) lives in
// src/client.cpp and is hidden behind this pointer, so httplib.h never
// leaks into consumers of <httpp/client.hpp> and stays compiled into
// the httpp .so/.a only.
class HTTPP_API Client {
public:
    explicit Client(const std::string& host, int port);
    ~Client();

    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    Response get(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace httpp
