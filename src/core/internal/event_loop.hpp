#pragma once

// Internal, NOT installed. The connection-level event loop of httpp::server,
// built on Asio (epoll on Linux, kqueue on macOS, IOCP/select on Windows).
//
// It accepts connections and watches the idle ones, all on the single thread
// that calls run(). An idle connection (waiting for its first or next request)
// therefore costs no worker thread. When one becomes readable, the loop
// *releases* the socket and hands it to `on_readable`; from then on the
// receiver owns it. It serves one request on a worker thread and either closes
// the socket or returns it with give_back() to be watched again.
//
// This header deliberately includes neither Asio nor cpp-httplib (both pull in
// the Windows socket headers, which conflict if mixed in one translation unit).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace httpp::detail {

// Wide enough for a POSIX fd (int) and a Windows SOCKET (UINT_PTR).
using native_socket = std::uintptr_t;

class event_loop {
public:
    struct options {
        int keep_alive_timeout_sec = 5;  // how long an idle connection is kept open
        std::size_t keep_alive_max = 100; // requests one connection may serve
    };
    // Called on the loop thread when an idle connection has data. The socket has
    // been released from the loop and is blocking-agnostic: the receiver owns it.
    // Must not block (hand the work to a thread pool).
    using readable_fn = std::function<void(native_socket, std::size_t remaining)>;
    // Called on the loop thread about twice a second (e.g. to shrink a thread pool).
    using tick_fn = std::function<void()>;

    event_loop(options opt, readable_fn on_readable, tick_fn on_tick = {});
    ~event_loop();
    event_loop(const event_loop&) = delete;
    event_loop& operator=(const event_loop&) = delete;

    // Opens, binds and listens. `port` 0 picks a free one. Returns the bound
    // port, or -1 on failure (address in use, bad host, no permission, ...).
    int bind(const std::string& host, int port);

    // Runs the loop on the calling thread until stop(). Returns false if the loop
    // was not bound or failed, true after a normal stop().
    bool run();

    // Thread-safe. Closes the listener and every idle connection, then run()
    // returns. A stop() requested after bind() but before run() is honoured.
    void stop();

    // Thread-safe. Hands a still-open connection back for its next request.
    // Returns false if the loop is not running; the caller must then close it.
    bool give_back(native_socket sock, std::size_t remaining);

    void set_keep_alive_timeout(int seconds);
    void set_keep_alive_max(std::size_t requests); // applies to connections accepted from now on

    // Thread-safe. Runs `fn` on the loop thread after `delay` (so `fn` must not block: hand real
    // work to a pool). Pending timers are dropped when the loop stops. Does nothing if the loop
    // is not running.
    void after(std::chrono::milliseconds delay, std::function<void()> fn);

    // False once the platform turned out unable to take a socket back into the
    // loop (Windows older than 8.1, or Wine): from then on every connection is told
    // `remaining == 1`, so its response says `Connection: close` and it is closed
    // after one request. Requests are still served and idle connections still cost no thread.
    bool keep_alive_supported() const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httpp::detail
