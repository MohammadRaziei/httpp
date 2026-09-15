#pragma once

#include "httpp/export.hpp"
#include "httpp/client.hpp" // reuses httpp::response for the outgoing side too

#include <functional>
#include <memory>
#include <string>

namespace httpp {

// Minimal per-request info handed to a route handler.
struct request {
    std::string method;
    std::string path;
    std::string body;
};

using handler = std::function<void(const request&, response&)>;

// httpp::server is what replaces "spin up libcurl/httplib yourself" for the
// server side: register routes or serve a directory (the httpp CLI's
// `httpp server` command, analogous to `python -m http.server`, is a thin
// wrapper around this class). Backed by vendored cpp-httplib, but that
// dependency is hidden behind this pointer (see src/core/server.cpp) and
// never appears in this public header.
class server {
public:
    HTTPP_API server();
    HTTPP_API ~server();

    HTTPP_API server(server&&) noexcept;
    HTTPP_API server& operator=(server&&) noexcept;
    server(const server&) = delete;
    server& operator=(const server&) = delete;

    HTTPP_API void get(const std::string& path, handler h);
    HTTPP_API void serve_directory(const std::string& mount_path, const std::string& local_dir);

    HTTPP_API int bind_to_any_port(const std::string& host);
    HTTPP_API void listen_after_bind();
    HTTPP_API void listen(const std::string& host, int port);
    HTTPP_API void stop();

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httpp
