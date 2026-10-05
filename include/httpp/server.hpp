#pragma once

#include "httpp/export.hpp"
#include "httpp/client.hpp" // reuses httpp::response for the outgoing side too

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
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

    // First match, "" if absent. Header names are case-insensitive.
    std::string header(const std::string& name) const {
        for (const auto& [k, v] : headers) {
            if (k.size() == name.size() &&
                std::equal(k.begin(), k.end(), name.begin(), [](unsigned char a, unsigned char b) {
                    return std::tolower(a) == std::tolower(b);
                }))
                return v;
        }
        return "";
    }
    std::string query_param(const std::string& name) const {
        for (const auto& [k, v] : query) if (k == name) return v;
        return "";
    }
    std::string path_param(const std::string& name) const {
        for (const auto& [k, v] : path_params) if (k == name) return v;
        return "";
    }
    // From the Cookie header ("a=1; b=2"); "" if absent.
    std::string cookie(const std::string& name) const {
        const std::string all = header("Cookie");
        std::size_t pos = 0;
        while (pos < all.size()) {
            std::size_t end = all.find(';', pos);
            if (end == std::string::npos) end = all.size();
            std::size_t start = pos;
            while (start < end && all[start] == ' ') start++;
            const std::size_t eq = all.find('=', start);
            if (eq != std::string::npos && eq < end && all.compare(start, eq - start, name) == 0)
                return all.substr(eq + 1, end - eq - 1);
            pos = end + 1;
        }
        return "";
    }
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
//  - On POSIX the connection is kept alive after an async response (if the
//    client allows it); on Windows it is closed after each async response.
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
    // An exception thrown by the handler is answered with a plain 500 (or goes to the exception
    // handler, see set_exception_handler). If no answer is sent within set_async_timeout() seconds
    // (default 60), the client gets a 504 and a later send() does nothing.
    HTTPP_API void route_async(const std::string& method, const std::string& path, async_handler h);
    HTTPP_API void get_async(const std::string& path, async_handler h);
    HTTPP_API void post_async(const std::string& path, async_handler h);
    HTTPP_API void put_async(const std::string& path, async_handler h);
    HTTPP_API void patch_async(const std::string& path, async_handler h);
    HTTPP_API void del_async(const std::string& path, async_handler h);
    HTTPP_API void serve_directory(const std::string& mount_path, const std::string& local_dir);

    HTTPP_API int bind_to_any_port(const std::string& host);
    // Block until stop(). Return false if the socket could not be bound/listened on
    // (e.g. port already in use), true after a normal stop().
    HTTPP_API bool listen_after_bind();
    HTTPP_API bool listen(const std::string& host, int port);
    HTTPP_API void stop();

    // --- Hooks. They run for every request, including static files and unmatched paths. ---
    // before: return true to answer the request yourself (the route is then not called), e.g. to
    //   reject it. The response starts empty with status 0 (sent as 200 unless you set it).
    // after: may change the response's status and headers on the way out (the body is not passed
    //   and changes to it are ignored). It also runs for async responses, when they are sent.
    // error handler: fill in the body for a 4xx/5xx the server produced itself (404, 405, ...).
    // exception handler: called when a route handler throws; the response starts as a 500.
    using before_hook = std::function<bool(const request&, response&)>;
    using after_hook = std::function<void(const request&, response&)>;
    using error_hook = std::function<void(const request&, response&)>;
    using exception_hook = std::function<void(const request&, response&, std::exception_ptr)>;
    HTTPP_API server& before(before_hook hook);
    HTTPP_API server& after(after_hook hook);
    HTTPP_API server& set_error_handler(error_hook hook);
    HTTPP_API server& set_exception_handler(exception_hook hook);

    // --- Settings; call them before listening. Each returns *this. ---
    // Worker threads for sync handlers: `base` are kept, up to `max` are used under load
    // (default max = 4 x base; default pool: about one thread per core, at least 8).
    HTTPP_API server& set_thread_pool(std::size_t base, std::size_t max = 0);
    HTTPP_API server& set_read_timeout(int seconds);        // reading a request (default 5)
    HTTPP_API server& set_write_timeout(int seconds);       // writing a response (default 5)
    HTTPP_API server& set_keep_alive_timeout(int seconds);  // idle keep-alive connection (default 5)
    HTTPP_API server& set_keep_alive_max(std::size_t requests); // requests per connection (default 100)
    HTTPP_API server& set_max_body(std::size_t bytes);      // largest request body accepted; 0 = no limit
    HTTPP_API server& set_async_timeout(int seconds);       // route_async: answer within this or 504; 0 = never

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httpp
