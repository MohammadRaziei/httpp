#include "internal/event_loop.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif
#ifndef ASIO_STANDALONE
#  define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <system_error>
#include <unordered_set>

namespace httpp::detail {
namespace {

using asio::ip::tcp;

constexpr native_socket invalid_socket = ~native_socket(0);

void close_native(native_socket fd) noexcept {
    if (fd == invalid_socket) return;
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(static_cast<int>(fd));
#endif
}

// Closes the socket if the handler that carries it is destroyed without running
// (e.g. the io_context is torn down with the hand-back still queued).
struct owned_socket {
    native_socket fd;
    explicit owned_socket(native_socket f) : fd(f) {}
    owned_socket(const owned_socket&) = delete;
    owned_socket& operator=(const owned_socket&) = delete;
    ~owned_socket() { close_native(fd); }
    native_socket take() {
        native_socket f = fd;
        fd = invalid_socket;
        return f;
    }
};

// Which address family does this socket use? (needed to adopt it into Asio)
tcp protocol_of(native_socket fd) {
    sockaddr_storage ss{};
#ifdef _WIN32
    int len = static_cast<int>(sizeof(ss));
    ::getsockname(static_cast<SOCKET>(fd), reinterpret_cast<sockaddr*>(&ss), &len);
#else
    socklen_t len = sizeof(ss);
    ::getsockname(static_cast<int>(fd), reinterpret_cast<sockaddr*>(&ss), &len);
#endif
    return ss.ss_family == AF_INET6 ? tcp::v6() : tcp::v4();
}

// Takes the socket out of Asio so a worker thread can use it with plain blocking
// calls. Returns invalid_socket on failure (the socket then stays Asio's).
native_socket release_socket(tcp::socket& sock, [[maybe_unused]] std::atomic<bool>& keep_alive_ok) {
    std::error_code ec;
    const auto fd = static_cast<native_socket>(sock.release(ec));
    if (!ec && fd != invalid_socket) return fd;
#ifdef _WIN32
    // release() re-associates the socket with another completion port, which needs
    // Windows 8.1 or later. Older systems (and Wine) answer operation_not_supported.
    // Fall back to the classic way: duplicate the handle, then drop ours. The
    // connection stays up until the last handle is closed.
    if (ec == asio::error::operation_not_supported && sock.is_open()) {
        WSAPROTOCOL_INFOW info;
        if (::WSADuplicateSocketW(sock.native_handle(), ::GetCurrentProcessId(), &info) == 0) {
            const SOCKET dup = ::WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
                                            &info, 0, WSA_FLAG_OVERLAPPED);
            if (dup != INVALID_SOCKET) {
                // The duplicate stays tied to this loop's completion port, so it can
                // never be adopted again: connections can no longer be kept alive.
                keep_alive_ok = false;
                std::error_code ignore;
                sock.close(ignore);
                return static_cast<native_socket>(dup);
            }
        }
    }
#endif
    return invalid_socket;
}

struct idle_conn {
    tcp::socket sock;
    asio::steady_timer timer;
    std::size_t remaining;
    idle_conn(asio::io_context& io, tcp::socket s, std::size_t rem)
        : sock(std::move(s)), timer(io), remaining(rem) {}
};

} // namespace

struct event_loop::impl {
    options opt;
    readable_fn on_readable;
    tick_fn on_tick;

    asio::io_context io;
    tcp::acceptor acceptor{io};
    asio::steady_timer backoff{io};
    asio::steady_timer ticker{io};
    std::atomic<int> keep_alive_sec;
    std::atomic<bool> keep_alive_ok{true};

    // Touched only on the loop thread.
    std::unordered_set<std::shared_ptr<idle_conn>> idle;
    bool closing = false;
    bool failed = false;

    // Guards the lifecycle flags below; also taken by give_back() so that a
    // hand-back is either accepted (and will be run) or refused (caller closes).
    std::mutex mu;
    bool bound = false;
    bool running = false;
    bool stop_requested = false;

    impl(options o, readable_fn r, tick_fn t)
        : opt(o), on_readable(std::move(r)), on_tick(std::move(t)), keep_alive_sec(o.keep_alive_timeout_sec) {}

    void begin_close() {
        if (closing) return;
        closing = true;
        std::error_code ec;
        acceptor.close(ec);
        backoff.cancel();
        ticker.cancel();
        auto all = std::move(idle);
        idle.clear();
        for (auto& c : all) {
            c->sock.close(ec);
            c->timer.cancel();
        }
    }

    void schedule_tick() {
        ticker.expires_after(std::chrono::milliseconds(500));
        ticker.async_wait([this](std::error_code ec) {
            if (ec || closing) return;
            if (on_tick) on_tick();
            schedule_tick();
        });
    }

    void accept_next() {
        acceptor.async_accept([this](std::error_code ec, tcp::socket s) {
            if (closing) return; // `s` closes itself
            if (!ec) {
                std::error_code ignore;
                s.set_option(tcp::no_delay(true), ignore);
                watch(std::make_shared<idle_conn>(io, std::move(s), opt.keep_alive_max));
                accept_next();
                return;
            }
            if (ec == asio::error::operation_aborted) return;
            if (!acceptor.is_open()) { // the listener is gone: nothing left to accept on
                failed = true;
                begin_close();
                return;
            }
            // Transient (aborted handshake) or resource exhaustion (out of fds):
            // pause briefly instead of spinning, then keep accepting.
            const bool exhausted = ec == asio::error::no_descriptors ||
                                   ec == asio::error::no_buffer_space || ec == asio::error::no_memory;
            backoff.expires_after(std::chrono::milliseconds(exhausted ? 20 : 1));
            backoff.async_wait([this](std::error_code e) {
                if (!e && !closing) accept_next();
            });
        });
    }

    // Watch an idle connection: hand it over when readable, close it on timeout.
    void watch(std::shared_ptr<idle_conn> c) {
        idle.insert(c);
        c->timer.expires_after(std::chrono::seconds(keep_alive_sec.load()));
        c->timer.async_wait([this, c](std::error_code ec) {
            if (ec) return; // cancelled: the connection was handed over or closed
            idle.erase(c);
            std::error_code ignore;
            c->sock.close(ignore); // also cancels the readiness wait below
        });
        c->sock.async_wait(asio::socket_base::wait_read, [this, c](std::error_code ec) {
            if (ec) return; // closed by the timeout or by shutdown
            c->timer.cancel();
            idle.erase(c);
            const native_socket fd = release_socket(c->sock, keep_alive_ok);
            if (fd == invalid_socket) return; // could not take it out of Asio; ~idle_conn closes it
            on_readable(fd, keep_alive_ok ? c->remaining : 1);
        });
    }
};

event_loop::event_loop(options opt, readable_fn on_readable, tick_fn on_tick)
    : impl_(std::make_unique<impl>(opt, std::move(on_readable), std::move(on_tick))) {}

event_loop::~event_loop() = default;

void event_loop::set_keep_alive_timeout(int seconds) { impl_->keep_alive_sec = seconds; }

bool event_loop::keep_alive_supported() const { return impl_->keep_alive_ok; }

int event_loop::bind(const std::string& host, int port) {
    auto& m = *impl_;
    std::error_code ec;

    auto address = asio::ip::make_address(host, ec);
    if (ec) { // not a literal address: resolve the name
        tcp::resolver resolver(m.io);
        auto results = resolver.resolve(host, "", ec);
        if (ec || results.empty()) return -1;
        address = results.begin()->endpoint().address();
    }
    const tcp::endpoint endpoint(address, static_cast<unsigned short>(port));

    if (m.acceptor.is_open()) m.acceptor.close(ec);
    m.acceptor.open(endpoint.protocol(), ec);
    if (ec) return -1;
#ifdef _WIN32
    // SO_REUSEADDR on Windows lets a second process take over a port that is
    // already served; ask for the opposite instead.
    const int one = 1;
    ::setsockopt(m.acceptor.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&one), sizeof(one));
#else
    // Fast restart after a stop (TIME_WAIT) -- but, unlike SO_REUSEPORT, a second
    // server cannot bind a port that is already being listened on.
    m.acceptor.set_option(asio::socket_base::reuse_address(true), ec);
#endif
    m.acceptor.bind(endpoint, ec);
    if (!ec) m.acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        std::error_code ignore;
        m.acceptor.close(ignore);
        return -1;
    }
    const auto bound_port = m.acceptor.local_endpoint(ec).port();
    if (ec) return -1;

    std::lock_guard<std::mutex> lock(m.mu);
    m.bound = true;
    m.stop_requested = false;
    return bound_port;
}

bool event_loop::run() {
    auto& m = *impl_;
    bool stop_now = false;
    {
        std::lock_guard<std::mutex> lock(m.mu);
        if (!m.bound || m.running) return false;
        m.running = true;
        stop_now = m.stop_requested;
    }
    m.closing = false;
    m.failed = false;
    m.io.restart();
    m.accept_next();
    m.schedule_tick();
    if (stop_now) asio::post(m.io, [&m] { m.begin_close(); });

    m.io.run(); // returns once the listener, timers and idle connections are all closed

    {
        std::lock_guard<std::mutex> lock(m.mu);
        m.running = false;
        m.bound = false;
    }
    // Run whatever was queued by give_back() before running became false; those
    // handlers see `closing` and close their sockets.
    m.io.restart();
    m.io.poll();
    return !m.failed;
}

void event_loop::stop() {
    auto& m = *impl_;
    std::lock_guard<std::mutex> lock(m.mu);
    if (!m.bound) return;
    m.stop_requested = true;
    if (m.running) asio::post(m.io, [&m] { m.begin_close(); });
}

bool event_loop::give_back(native_socket sock, std::size_t remaining) {
    auto& m = *impl_;
    std::lock_guard<std::mutex> lock(m.mu);
    if (!m.running) return false;
    auto guard = std::make_shared<owned_socket>(sock);
    asio::post(m.io, [&m, guard, remaining] {
        const native_socket fd = guard->take();
        if (m.closing) {
            close_native(fd);
            return;
        }
        tcp::socket s(m.io);
        std::error_code ec;
        s.assign(protocol_of(fd), static_cast<tcp::socket::native_handle_type>(fd), ec);
        if (ec) {
            close_native(fd);
            return;
        }
        m.watch(std::make_shared<idle_conn>(m.io, std::move(s), remaining));
    });
    return true;
}

} // namespace httpp::detail
