// A WebSocket load generator for the benchmark suite (oha does not speak WebSocket).
//
//   ws_load PORT [--path /ws] [--conns 64] [--seconds 5] [--warmup 1] [--size 64] [--threads 2] [--idle]
//
// Echo mode (default): every connection sends one masked text message of --size bytes, waits for the echo, and sends the next at once
// (a closed loop, like oha with --conns). After all connections are open and a warm-up, it counts echoes for --seconds and prints one
// JSON line: messages per second, latency percentiles, errors.
// Idle mode (--idle): opens --conns connections, sends nothing, prints {"connected": n, "failed": m} and holds them until its stdin is
// closed. The suite reads the server's memory in that window.
//
// ponytail: plain Asio callbacks, a fixed mask key (the server only needs the bit set), every sample kept in memory (4 bytes each).
// Upgrade path if it ever becomes the bottleneck: more --threads, or io_uring.
#include <asio.hpp>

#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

struct options {
    int port = 0;
    std::string path = "/ws";
    int conns = 64;
    double seconds = 5, warmup = 1;
    std::size_t size = 64;
    int threads = 2;
    bool idle = false;
};

struct shared_state {
    options opt;
    std::atomic<int> ready{0}, failed{0};
    std::atomic<bool> measuring{false};
    std::atomic<long long> errors{0};
    std::shared_ptr<const std::string> frame;  // one masked text frame, the same for every connection
};

struct worker {
    asio::io_context io;
    std::vector<std::uint32_t> samples;  // nanoseconds, only while measuring
    std::vector<int> pending;            // connections this worker still has to open, one at a time
    std::size_t next = 0;
};

// The 24-character key of RFC 6455 section 1.3: any valid key does.
const char* const ws_key = "dGhlIHNhbXBsZSBub25jZQ==";

class conn : public std::enable_shared_from_this<conn> {
public:
    conn(shared_state& st, worker& w, std::function<void()> opened)
        : st_(st), w_(w), sock_(w.io), opened_(std::move(opened)) {}

    void start() {
        sock_.async_connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(st_.opt.port)),
                            [self = shared_from_this()](std::error_code ec) {
                                if (ec) return self->fail("connect: " + ec.message());
                                self->sock_.set_option(asio::ip::tcp::no_delay(true));
                                self->handshake();
                            });
    }

private:
    void fail(const std::string& why) {
        if (!up_) {
            st_.failed.fetch_add(1);
            if (st_.failed.load() <= 3) std::fprintf(stderr, "ws_load: %s\n", why.c_str());
            opened_();  // the chain goes on, a failed connection is counted
        } else if (!closing_) {
            st_.errors.fetch_add(1);
            if (st_.errors.load() <= 3) std::fprintf(stderr, "ws_load: %s\n", why.c_str());
        }
        closing_ = true;
        std::error_code ignore;
        sock_.close(ignore);
    }

    void handshake() {
        req_ = "GET " + st_.opt.path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(st_.opt.port) +
               "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + ws_key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        asio::async_write(sock_, asio::buffer(req_), [self = shared_from_this()](std::error_code ec, std::size_t) {
            if (ec) return self->fail("handshake write: " + ec.message());
            self->read_head();
        });
    }

    void read_head() {
        sock_.async_read_some(asio::buffer(rbuf_), [self = shared_from_this()](std::error_code ec, std::size_t n) {
            if (ec) return self->fail("handshake read: " + ec.message());
            self->in_.append(self->rbuf_.data(), n);
            const auto end = self->in_.find("\r\n\r\n");
            if (end == std::string::npos) return self->read_head();
            if (self->in_.compare(0, 12, "HTTP/1.1 101") != 0) return self->fail("handshake refused: " + self->in_.substr(0, self->in_.find('\r')));
            self->in_.erase(0, end + 4);
            self->up_ = true;
            self->st_.ready.fetch_add(1);
            self->opened_();
            if (!self->st_.opt.idle) self->send();
            self->read_frames();
        });
    }

    void send() {
        if (closing_) return;
        t_send_ = clock_type::now();
        asio::async_write(sock_, asio::buffer(*st_.frame), [self = shared_from_this()](std::error_code ec, std::size_t) {
            if (ec) self->fail("write: " + ec.message());
        });
    }

    void read_frames() {
        sock_.async_read_some(asio::buffer(rbuf_), [self = shared_from_this()](std::error_code ec, std::size_t n) {
            if (ec) return self->fail("read: " + ec.message());
            self->in_.append(self->rbuf_.data(), n);
            self->parse();
        });
    }

    void parse() {
        for (;;) {
            if (in_.size() < 2) break;
            const unsigned b0 = static_cast<unsigned char>(in_[0]), b1 = static_cast<unsigned char>(in_[1]);
            std::size_t len = b1 & 0x7f, at = 2;
            if (len == 126) {
                if (in_.size() < 4) break;
                len = (static_cast<unsigned char>(in_[2]) << 8) | static_cast<unsigned char>(in_[3]);
                at = 4;
            } else if (len == 127) {
                if (in_.size() < 10) break;
                len = 0;
                for (int k = 0; k < 8; ++k) len = (len << 8) | static_cast<unsigned char>(in_[2 + k]);
                at = 10;
            }
            if (b1 & 0x80) return fail("the server sent a masked frame");
            if (in_.size() < at + len) break;
            const unsigned op = b0 & 0x0f;
            in_.erase(0, at + len);
            if (op == 8) return fail("the server closed the connection");
            if (op != 1 && op != 2) continue;  // ping, pong
            if (st_.opt.idle) continue;
            if (len != st_.opt.size) return fail("echo of " + std::to_string(len) + " bytes, sent " + std::to_string(st_.opt.size));
            if (st_.measuring.load(std::memory_order_relaxed))
                w_.samples.push_back(static_cast<std::uint32_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - t_send_).count()));
            send();
        }
        read_frames();
    }

    shared_state& st_;
    worker& w_;
    asio::ip::tcp::socket sock_;
    std::function<void()> opened_;
    std::array<char, 65536> rbuf_;
    std::string in_, req_;
    clock_type::time_point t_send_;
    bool up_ = false, closing_ = false;
};

// Opens this worker's connections one after the other, so that thousands of them do not overflow a listen queue.
void open_next(shared_state& st, worker& w) {
    if (w.next >= w.pending.size()) return;
    ++w.next;
    std::make_shared<conn>(st, w, [&st, &w] { asio::post(w.io, [&st, &w] { open_next(st, w); }); })->start();
}

std::string build_frame(std::size_t size) {
    std::string f;
    f.push_back(static_cast<char>(0x81));  // FIN, text
    const unsigned char key[4] = {0x37, 0xfa, 0x21, 0x3d};
    if (size < 126) {
        f.push_back(static_cast<char>(0x80 | size));
    } else if (size < 65536) {
        f.push_back(static_cast<char>(0x80 | 126));
        f.push_back(static_cast<char>(size >> 8));
        f.push_back(static_cast<char>(size & 0xff));
    } else {
        f.push_back(static_cast<char>(0x80 | 127));
        for (int k = 7; k >= 0; --k) f.push_back(static_cast<char>((static_cast<unsigned long long>(size) >> (8 * k)) & 0xff));
    }
    f.append(reinterpret_cast<const char*>(key), 4);
    for (std::size_t i = 0; i < size; ++i) f.push_back(static_cast<char>('x' ^ key[i & 3]));  // the payload is size times 'x', valid UTF-8
    return f;
}

void raise_fd_limit() {
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

} // namespace

int main(int argc, char** argv) {
    shared_state st;
    options& o = st.opt;
    if (argc < 2) {
        std::fprintf(stderr, "usage: ws_load PORT [--path /ws] [--conns N] [--seconds S] [--warmup S] [--size B] [--threads T] [--idle]\n");
        return 2;
    }
    o.port = std::atoi(argv[1]);
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : (std::fprintf(stderr, "ws_load: %s needs a value\n", a.c_str()), std::exit(2), nullptr); };
        if (a == "--path") o.path = value();
        else if (a == "--conns") o.conns = std::atoi(value());
        else if (a == "--seconds") o.seconds = std::atof(value());
        else if (a == "--warmup") o.warmup = std::atof(value());
        else if (a == "--size") o.size = static_cast<std::size_t>(std::atoll(value()));
        else if (a == "--threads") o.threads = std::max(1, std::atoi(value()));
        else if (a == "--idle") o.idle = true;
        else { std::fprintf(stderr, "ws_load: unknown option %s\n", a.c_str()); return 2; }
    }
    raise_fd_limit();
    st.frame = std::make_shared<const std::string>(build_frame(o.size));

    const int nthreads = std::min(o.threads, std::max(1, o.conns));
    std::vector<std::unique_ptr<worker>> workers;
    for (int t = 0; t < nthreads; ++t) workers.push_back(std::make_unique<worker>());
    for (int c = 0; c < o.conns; ++c) workers[static_cast<std::size_t>(c % nthreads)]->pending.push_back(c);

    std::vector<std::thread> threads;
    std::vector<asio::executor_work_guard<asio::io_context::executor_type>> guards;
    for (auto& w : workers) guards.emplace_back(asio::make_work_guard(w->io));
    for (auto& w : workers) {
        asio::post(w->io, [&st, &w] { open_next(st, *w); });
        threads.emplace_back([&w] { w->io.run(); });
    }

    const auto up_to = clock_type::now() + std::chrono::seconds(o.idle ? 120 : 20);
    while (st.ready.load() + st.failed.load() < o.conns && clock_type::now() < up_to) std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int rc = 0;
    if (o.idle) {
        std::printf("{\"connected\": %d, \"failed\": %d}\n", st.ready.load(), st.failed.load());
        std::fflush(stdout);
        char sink[64];
        while (std::fread(sink, 1, sizeof sink, stdin) > 0) {}  // held until the suite closes our stdin
    } else {
        std::this_thread::sleep_for(std::chrono::duration<double>(o.warmup));
        st.measuring = true;
        const auto t0 = clock_type::now();
        std::printf("{\"started\": true}\n");  // the suite reads the server's CPU time now
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::duration<double>(o.seconds));
        st.measuring = false;
        const double elapsed = std::chrono::duration<double>(clock_type::now() - t0).count();
        for (auto& g : guards) g.reset();
        for (auto& w : workers) w->io.stop();
        for (auto& t : threads) t.join();
        threads.clear();
        std::vector<std::uint32_t> all;
        for (auto& w : workers) all.insert(all.end(), w->samples.begin(), w->samples.end());
        std::sort(all.begin(), all.end());
        auto pct = [&](double p) { return all.empty() ? 0.0 : all[std::min(all.size() - 1, static_cast<std::size_t>(p * static_cast<double>(all.size())))] / 1e6; };
        std::printf("{\"msgs_per_s\": %.1f, \"p50_ms\": %.4f, \"p99_ms\": %.4f, \"p999_ms\": %.4f, \"connected\": %d, \"failed\": %d, \"errors\": %lld}\n",
                    static_cast<double>(all.size()) / elapsed, pct(0.50), pct(0.99), pct(0.999), st.ready.load(), st.failed.load(), st.errors.load());
        rc = all.empty() ? 1 : 0;
    }
    for (auto& g : guards) g.reset();
    for (auto& w : workers) w->io.stop();
    for (auto& t : threads) t.join();
    return rc;
}
