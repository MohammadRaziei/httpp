#pragma once

// Internal, NOT installed. The HTTP/1.1 server engine behind httpp::server: Asio does all socket
// I/O on ONE thread (epoll on Linux, kqueue on macOS, IOCP on Windows) and llhttp parses requests.
// Accepting, reading, idle keep-alive connections, timeouts and writing never occupy a worker
// thread. Code that may block (route handlers, file/stream body providers) runs on a pool owned
// by the caller; the engine only ever asks the caller to run it.
//
// Like async_http.hpp this header includes neither Asio nor cpp-httplib.

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace httpp::detail {

struct http_request {
    std::string method;   // "GET", "POST", ...
    std::string target;   // raw request target: "/path?query" (not decoded)
    int version_minor = 1;
    std::vector<std::pair<std::string, std::string>> headers; // wire order
    std::string body;     // whole body, de-chunked
};

// What the engine writes back. It adds Content-Length / Transfer-Encoding, Connection and Date.
struct wire_response {
    int status = 200;                       // 0 = send nothing and close the connection
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;                       // used when `next` is empty
    // Streaming body: `next` appends the next piece to its argument and returns true while there
    // is more (the piece may be empty); false means the body ended (the piece it appended on
    // that last call still counts). It runs on a pool thread, one call at a time. Throwing aborts
    // the response: the connection is dropped, never ended cleanly. `length` is the total size if
    // known (then Content-Length is sent), else -1 (chunked).
    std::function<bool(std::string&)> next;
    long long length = -1;
    bool close = false;                     // end the connection after this response
};

class async_server {
public:
    struct options {
        int read_timeout_sec = 5;        // between reads while a request arrives
        int write_timeout_sec = 5;       // per write of a response
        int keep_alive_timeout_sec = 5;  // idle connection waiting for its next request
        std::size_t keep_alive_max = 100; // requests one connection may serve
        std::size_t max_body = 100u * 1024 * 1024; // 0 = no limit
    };

    // `answer` may be called from any thread, once; later calls and calls after the connection or
    // the server is gone do nothing.
    using answer_fn = std::function<void(wire_response)>;
    // Called on the loop thread for every complete request: must not block. If it returns false
    // (e.g. the pool is shutting down) the connection is dropped.
    using request_fn = std::function<bool(http_request, answer_fn)>;
    // Runs `job` (which may block) on a pool thread; false if there is none.
    using blocking_fn = std::function<bool(std::function<void()>)>;

    async_server(options opt, request_fn on_request, blocking_fn run_blocking);
    ~async_server();
    async_server(const async_server&) = delete;
    async_server& operator=(const async_server&) = delete;

    // Opens, binds, listens. `port` 0 picks a free one. Returns the bound port or -1.
    int bind(const std::string& host, int port);
    // Runs the loop on the calling thread until stop(). False if not bound or the listener failed.
    bool run();
    // Thread-safe. Closes the listener and every connection; run() then returns. A stop()
    // requested after bind() but before run() is honoured.
    void stop();

    // Thread-safe. Runs `fn` on the loop thread after `delay`; it must not block. Dropped when the
    // loop stops. Does nothing if the loop is not running.
    void after(std::chrono::milliseconds delay, std::function<void()> fn);

    // Settings: call before run().
    void set_read_timeout(int seconds);
    void set_write_timeout(int seconds);
    void set_keep_alive_timeout(int seconds);
    void set_keep_alive_max(std::size_t requests);
    void set_max_body(std::size_t bytes);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httpp::detail
