// Exercises the internal Asio event loop on its own, with plain sockets standing in
// for the worker pool. It runs on every platform (epoll / kqueue / IOCP), so it is
// what tells us the connection layer really works on Linux, macOS and Windows.
// Windows: utest.h includes <Windows.h>. Without WIN32_LEAN_AND_MEAN the Windows SDK then pulls in the
// legacy <winsock.h>, which clashes with <winsock2.h> ("'sockaddr': 'struct' type redefinition").
// So: lean headers first, and <winsock2.h> before utest.h.
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#endif

#include "internal/event_loop.hpp"

#include "utest/utest.h"

#ifdef _WIN32
using test_sock = SOCKET;
static const test_sock bad_sock = INVALID_SOCKET;
static void close_sock(test_sock s) { closesocket(s); }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using test_sock = int;
static const test_sock bad_sock = -1;
static void close_sock(test_sock s) { close(s); }
#endif

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using httpp::detail::event_loop;
using httpp::detail::native_socket;
using namespace std::chrono_literals;

namespace {

struct winsock_init {
    winsock_init() {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
    }
};
const winsock_init winsock_once;

test_sock connect_loopback(int port) {
    test_sock s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        close_sock(s);
        return bad_sock;
    }
#ifdef _WIN32
    DWORD timeout_ms = 3000; // a hang must fail the test, not stall it
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{3, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return s;
}

std::string receive(test_sock s) {
    char buf[256];
    int n = recv(s, buf, sizeof(buf), 0);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

bool closed_by_peer(test_sock s) {
    char c;
    return recv(s, &c, 1, 0) == 0;
}

// The "worker": reads what the client sent, answers "echo:<it>" with plain blocking
// calls, then hands the connection back unless it has no requests left -- the same
// decisions httpp::server makes.
class echo_server {
public:
    explicit echo_server(int keep_alive_timeout_sec = 5) {
        event_loop::options opt;
        opt.keep_alive_timeout_sec = keep_alive_timeout_sec;
        loop_ = std::make_unique<event_loop>(opt, [this](native_socket fd, std::size_t remaining) {
            handed_++;
            std::lock_guard<std::mutex> lock(mutex_);
            workers_.emplace_back([this, fd, remaining] { serve(fd, remaining); });
        });
        port = loop_->bind("127.0.0.1", 0);
        runner_ = std::thread([this] { run_ok_ = loop_->run(); });
        std::this_thread::sleep_for(100ms);
    }
    ~echo_server() { shutdown(); }
    void shutdown() {
        if (!runner_.joinable()) return;
        loop_->stop();
        runner_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& t : workers_) t.join();
        workers_.clear();
    }

    int port = -1;
    event_loop& loop() { return *loop_; }
    int handed() const { return handed_; }
    bool run_ok() const { return run_ok_; }

private:
    void serve(native_socket fd, std::size_t remaining) {
        test_sock s = static_cast<test_sock>(fd);
#ifdef _WIN32
        u_long blocking = 0; // what httplib's prepare_socket does to a socket the loop gave away
        ioctlsocket(s, FIONBIO, &blocking);
#endif
        char buf[256];
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) { close_sock(s); return; }
        std::string reply = "echo:" + std::string(buf, static_cast<size_t>(n));
        send(s, reply.data(), static_cast<int>(reply.size()), 0);
        if (remaining <= 1 || !loop_->give_back(fd, remaining - 1)) close_sock(s);
    }

    std::unique_ptr<event_loop> loop_;
    std::thread runner_;
    std::mutex mutex_;
    std::vector<std::thread> workers_;
    std::atomic<int> handed_{0};
    std::atomic<bool> run_ok_{false};
};

} // namespace

UTEST(httpp_event_loop, serves_a_request_on_an_idle_connection) {
    echo_server srv;
    ASSERT_TRUE(srv.port > 0);
    test_sock c = connect_loopback(srv.port);
    ASSERT_TRUE(c != bad_sock);
    send(c, "one", 3, 0);
    ASSERT_STREQ("echo:one", receive(c).c_str());
    close_sock(c);
}

UTEST(httpp_event_loop, keeps_the_connection_alive_where_the_platform_allows) {
    echo_server srv;
    test_sock c = connect_loopback(srv.port);
    ASSERT_TRUE(c != bad_sock);
    send(c, "one", 3, 0);
    ASSERT_STREQ("echo:one", receive(c).c_str());
    if (srv.loop().keep_alive_supported()) {
        // epoll/kqueue, and Windows 8.1+: the same connection serves request after request.
        for (const char* msg : {"two", "three", "four"}) {
            send(c, msg, static_cast<int>(std::string(msg).size()), 0);
            ASSERT_STREQ((std::string("echo:") + msg).c_str(), receive(c).c_str());
        }
        ASSERT_EQ(4, srv.handed());
    } else {
        // Older Windows / Wine: honest degradation. The response is the last on this
        // connection, and a new connection is served just as well.
        ASSERT_TRUE(closed_by_peer(c));
        close_sock(c);
        c = connect_loopback(srv.port);
        send(c, "two", 3, 0);
        ASSERT_STREQ("echo:two", receive(c).c_str());
    }
    close_sock(c);
}

UTEST(httpp_event_loop, idle_connections_hand_nothing_to_workers) {
    echo_server srv;
    std::vector<test_sock> idle;
    for (int i = 0; i < 200; i++) idle.push_back(connect_loopback(srv.port));
    std::this_thread::sleep_for(300ms);
    ASSERT_EQ(0, srv.handed()); // 200 open connections, not one worker used

    test_sock c = connect_loopback(srv.port);
    send(c, "ping", 4, 0);
    ASSERT_STREQ("echo:ping", receive(c).c_str()); // and a real request is still answered at once
    ASSERT_EQ(1, srv.handed());
    close_sock(c);
    for (auto s : idle) close_sock(s);
}

UTEST(httpp_event_loop, idle_connection_is_closed_after_the_keep_alive_timeout) {
    echo_server srv(1);
    test_sock c = connect_loopback(srv.port);
    ASSERT_TRUE(c != bad_sock);
    std::this_thread::sleep_for(1600ms);
    ASSERT_TRUE(closed_by_peer(c));
    ASSERT_EQ(0, srv.handed());
    close_sock(c);
}

UTEST(httpp_event_loop, a_second_loop_cannot_bind_a_port_that_is_being_served) {
    echo_server srv;
    event_loop other({}, [](native_socket, std::size_t) {});
    ASSERT_TRUE(other.bind("127.0.0.1", srv.port) < 0);
}

UTEST(httpp_event_loop, stop_requested_before_run_is_honoured) {
    event_loop loop({}, [](native_socket, std::size_t) {});
    ASSERT_TRUE(loop.bind("127.0.0.1", 0) > 0);
    loop.stop();
    ASSERT_TRUE(loop.run()); // returns at once instead of serving forever
}

UTEST(httpp_event_loop, give_back_is_refused_when_the_loop_is_not_running) {
    event_loop loop({}, [](native_socket, std::size_t) {});
    ASSERT_FALSE(loop.give_back(static_cast<native_socket>(0), 1)); // caller keeps (and closes) the socket
}
