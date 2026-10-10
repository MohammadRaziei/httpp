#include "internal/async_http.hpp"

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

#include "llhttp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <unordered_set>

namespace httpp::detail {
namespace {

using asio::ip::tcp;

bool ieq(const std::string& a, const char* b) {
    std::size_t n = std::char_traits<char>::length(b);
    return a.size() == n && std::equal(a.begin(), a.end(), b, [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}

bool has_header(const async_job& j, const char* name) {
    for (const auto& h : j.headers) if (ieq(h.first, name)) return true;
    return false;
}

// Header injection guard: nothing that ends up on the wire may contain CR or LF.
bool clean(const std::string& s, bool allow_space) {
    for (unsigned char c : s) {
        if (c == '\r' || c == '\n' || c == 0) return false;
        if (!allow_space && c == ' ') return false;
    }
    return true;
}

} // namespace

struct async_http::impl {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    std::thread thread;

    struct exchange;
    std::unordered_set<std::shared_ptr<exchange>> live; // loop thread only

    impl() : thread([this] { io.run(); }) {}

    // One request/response. Lives on the loop thread; owned by `live` until it finishes.
    struct exchange : std::enable_shared_from_this<exchange> {
        impl& owner;
        async_job job;
        done_fn done;

        tcp::resolver resolver;
        tcp::socket sock;
        asio::steady_timer timer;
        std::string out;          // the request bytes
        std::string rbuf = std::string(16384, '\0');

        llhttp_t parser;
        llhttp_settings_t settings;
        response res;
        bool last_was_value = true;
        bool finished = false;
        bool message_done = false;
        bool informational = false; // current message is a 1xx we skip
        std::uint64_t armed = 0;    // generation, so a stale timer callback is ignored

        exchange(impl& o, async_job j, done_fn d)
            : owner(o), job(std::move(j)), done(std::move(d)),
              resolver(o.io), sock(o.io), timer(o.io) {
            llhttp_settings_init(&settings);
            settings.on_header_field = [](llhttp_t* p, const char* at, std::size_t n) -> int {
                auto* e = static_cast<exchange*>(p->data);
                if (e->last_was_value) e->res.headers.emplace_back();
                e->last_was_value = false;
                e->res.headers.back().first.append(at, n);
                return 0;
            };
            settings.on_header_value = [](llhttp_t* p, const char* at, std::size_t n) -> int {
                auto* e = static_cast<exchange*>(p->data);
                e->last_was_value = true;
                e->res.headers.back().second.append(at, n);
                return 0;
            };
            settings.on_headers_complete = [](llhttp_t* p) -> int {
                auto* e = static_cast<exchange*>(p->data);
                int code = p->status_code;
                if (code >= 100 && code < 200 && code != 101) { // 100 Continue etc.: the real one follows
                    e->informational = true;
                    return 0;
                }
                e->res.status = code;
                return ieq(e->job.method, "HEAD") ? 1 : 0; // 1: this response has no body
            };
            settings.on_body = [](llhttp_t* p, const char* at, std::size_t n) -> int {
                auto* e = static_cast<exchange*>(p->data);
                if (e->job.max_response_size && e->res.body.size() + n > e->job.max_response_size) {
                    e->res.status = 413;
                    e->fail(error_kind::too_large, "response body exceeds max_response_size");
                    return HPE_USER;
                }
                e->res.body.append(at, n);
                return 0;
            };
            settings.on_message_complete = [](llhttp_t* p) -> int {
                auto* e = static_cast<exchange*>(p->data);
                if (e->informational) { // discard the 1xx head and keep waiting
                    e->informational = false;
                    e->res.headers.clear();
                    e->last_was_value = true;
                    return 0;
                }
                e->message_done = true;
                return 0;
            };
            llhttp_init(&parser, HTTP_RESPONSE, &settings);
            parser.data = this;
        }

        void abort_now(error_kind k, const std::string& msg) { fail(k, msg); }

        // Ends the exchange (once). Closes the socket, tells the caller.
        void fail(error_kind k, const std::string& msg) {
            if (finished) return;
            std::error_code ignored;
            sock.close(ignored);
            res.error = k;
            res.error_message = msg;
            if (k != error_kind::too_large) res.status = 0;
            complete();
        }
        void succeed() {
            if (finished) return;
            std::error_code ignored;
            sock.close(ignored);
            complete();
        }
        void complete() {
            finished = true;
            timer.cancel();
            auto self = shared_from_this();
            owner.live.erase(self);
            if (done) { auto d = std::move(done); d(std::move(res)); }
        }

        void arm() {
            if (job.timeout_sec <= 0) return;
            std::uint64_t gen = ++armed;
            timer.expires_after(std::chrono::seconds(job.timeout_sec));
            timer.async_wait([self = shared_from_this(), gen](std::error_code ec) {
                if (ec || self->finished || gen != self->armed) return;
                self->fail(error_kind::timeout, "timed out");
            });
        }
        void disarm() { ++armed; timer.cancel(); }

        void start() {
            if (!clean(job.method, false) || !clean(job.target, false) || !clean(job.host, false) ||
                job.method.empty() || job.target.empty()) {
                return fail(error_kind::other, "invalid request line");
            }
            for (const auto& h : job.headers) {
                if (h.first.empty() || !clean(h.first, false) || !clean(h.second, true) ||
                    h.first.find(':') != std::string::npos) {
                    return fail(error_kind::other, "invalid header");
                }
            }
            out = job.method + " " + job.target + " HTTP/1.1\r\n";
            if (!has_header(job, "Host")) {
                out += "Host: " + job.host;
                if (job.port != 80) out += ":" + std::to_string(job.port);
                out += "\r\n";
            }
            for (const auto& h : job.headers) out += h.first + ": " + h.second + "\r\n";
            if (!has_header(job, "Connection")) out += "Connection: close\r\n";
            bool wants_len = !job.body.empty() || ieq(job.method, "POST") || ieq(job.method, "PUT") ||
                             ieq(job.method, "PATCH");
            if (wants_len && !has_header(job, "Content-Length"))
                out += "Content-Length: " + std::to_string(job.body.size()) + "\r\n";
            out += "\r\n";
            out += job.body;

            arm();
            resolver.async_resolve(job.host, std::to_string(job.port),
                [self = shared_from_this()](std::error_code ec, tcp::resolver::results_type eps) {
                    if (self->finished) return;
                    if (ec) return self->fail(error_kind::connection, "cannot resolve host: " + ec.message());
                    self->connect(std::move(eps));
                });
        }

        void connect(tcp::resolver::results_type eps) {
            arm();
            asio::async_connect(sock, eps,
                [self = shared_from_this()](std::error_code ec, const tcp::endpoint&) {
                    if (self->finished) return;
                    if (ec) return self->fail(error_kind::connection, "cannot connect: " + ec.message());
                    self->send();
                });
        }

        void send() {
            arm();
            asio::async_write(sock, asio::buffer(out),
                [self = shared_from_this()](std::error_code ec, std::size_t) {
                    if (self->finished) return;
                    if (ec) return self->fail(error_kind::connection, "write failed: " + ec.message());
                    self->read();
                });
        }

        void read() {
            arm();
            sock.async_read_some(asio::buffer(rbuf),
                [self = shared_from_this()](std::error_code ec, std::size_t n) {
                    if (self->finished) return;
                    self->on_read(ec, n);
                });
        }

        void on_read(std::error_code ec, std::size_t n) {
            if (n > 0) {
                llhttp_errno_t r = llhttp_execute(&parser, rbuf.data(), n);
                if (finished) return; // too_large already reported from the body callback
                if (r != HPE_OK && r != HPE_PAUSED_UPGRADE) {
                    return fail(error_kind::other, std::string("malformed response: ") + llhttp_errno_name(r));
                }
                if (message_done) return succeed();
            }
            if (!ec) return read();
            disarm();
            if (ec == asio::error::eof) { // a body that ends at connection close is legal
                llhttp_errno_t r = llhttp_finish(&parser);
                if (r == HPE_OK && (message_done || res.status != 0)) return succeed();
                return fail(error_kind::connection, "connection closed before the response was complete");
            }
            fail(error_kind::connection, "read failed: " + ec.message());
        }
    };
};

async_http::async_http() : impl_(std::make_unique<impl>()) {}

async_http::~async_http() {
    // Cancel what is in flight on the loop thread, then let the loop drain and exit.
    asio::post(impl_->io, [this] {
        auto copy = impl_->live;
        for (auto& e : copy) e->abort_now(error_kind::canceled, "client destroyed");
    });
    impl_->guard.reset();
    if (impl_->thread.joinable()) impl_->thread.join();
}

void async_http::submit(async_job job, done_fn done) {
    asio::post(impl_->io, [this, job = std::move(job), done = std::move(done)]() mutable {
        auto ex = std::make_shared<impl::exchange>(*impl_, std::move(job), std::move(done));
        impl_->live.insert(ex);
        ex->start();
    });
}

} // namespace httpp::detail
