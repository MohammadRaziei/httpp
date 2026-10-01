#pragma once

#include "httpp/export.hpp"
#include "httpp/client.hpp" // reuses httpp::response for the outgoing side too

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace httpp {

// Minimal per-request info handed to a route handler.
struct request {
    std::string method;
    std::string path;
    std::string body;
    // Repeated names are kept (in wire order), like response::headers.
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::pair<std::string, std::string>> query;        // ?a=1&b=2
    std::vector<std::pair<std::string, std::string>> path_params;  // "/users/:id"
};

using handler = std::function<void(const request&, response&)>;

// Asynchronous ("deferred") responses. An async handler gets a `responder`
// instead of a response to fill in. It may return immediately: the worker
// thread is released at once, the connection stays open, and the response is
// sent whenever `responder::send` is called, from any thread. This is what lets
// an event loop answer many requests at once without one blocked thread each.
//
//  - send() is thread-safe; only the first call has any effect, and it may
//    happen before the handler has returned.
//  - The connection is closed after an async response (no keep-alive).
//  - If the server is destroyed first, pending connections are closed and a
//    later send() does nothing. Dropping every copy of a responder without
//    calling send() closes the connection without a response.
class responder {
public:
    HTTPP_API void send(response res) const;

private:
    friend class server;
    struct impl;
    std::shared_ptr<impl> impl_;
};

using async_handler = std::function<void(const request&, responder)>;

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

    // `path` may contain ":name" segments ("/users/:id"), captured into
    // request::path_params. `method` is GET/POST/PUT/PATCH/DELETE/OPTIONS
    // (case-insensitive); anything else throws std::invalid_argument.
    // A handler may set a "Content-Type" entry in response::headers
    // (default: text/plain).
    HTTPP_API void route(const std::string& method, const std::string& path, handler h);
    // Shorthands for route("<METHOD>", ...). `del` because `delete` is a keyword.
    HTTPP_API void get(const std::string& path, handler h);
    HTTPP_API void post(const std::string& path, handler h);
    HTTPP_API void put(const std::string& path, handler h);
    HTTPP_API void patch(const std::string& path, handler h);
    HTTPP_API void del(const std::string& path, handler h);
    // Same as route(), but the handler answers through a `responder` (see above).
    // An exception thrown by the handler is answered with a plain 500.
    HTTPP_API void route_async(const std::string& method, const std::string& path, async_handler h);
    HTTPP_API void serve_directory(const std::string& mount_path, const std::string& local_dir);

    HTTPP_API int bind_to_any_port(const std::string& host);
    // Block until stop(). Return false if the socket could not be bound/listened on
    // (e.g. port already in use), true after a normal stop().
    HTTPP_API bool listen_after_bind();
    HTTPP_API bool listen(const std::string& host, int port);
    HTTPP_API void stop();

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httpp
