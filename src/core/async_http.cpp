#include "internal/async_http.hpp"
#include "internal/log.hpp"

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

#include "internal/tls_session.hpp"
#include "llhttp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <map>
#include <thread>
#include <tuple>
#include <unordered_set>

namespace httpp::detail {
namespace {

using asio::ip::tcp;

constexpr std::size_t max_idle_per_origin = 8;
constexpr int idle_timeout_sec = 60; // an idle pooled connection is closed after this long

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

bool idempotent(const std::string& m) {
    return ieq(m, "GET") || ieq(m, "HEAD") || ieq(m, "OPTIONS") || ieq(m, "PUT") || ieq(m, "DELETE");
}

// Outcome of one connection operation.
struct io_result {
    bool ok = true;
    bool eof = false;        // read: the peer closed the connection
    error_kind kind = error_kind::other;
    std::string message;
    std::string data;        // read: plaintext (after TLS decryption)
};

using key_t = std::tuple<std::string, int, bool, bool, std::string, std::string, int>;

} // namespace

struct async_http::impl {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    std::thread thread;

    struct connection;
    struct exchange;
    // Destruction order matters (reverse of declaration): idle connections go first, while `io` lives.
    std::map<key_t, std::deque<std::shared_ptr<connection>>> idle; // loop thread only
    std::unordered_set<std::shared_ptr<exchange>> live;             // loop thread only
    // Trust settings per (verify, ca_file). The system roots take tens of ms to parse: do it once.
    // ponytail: never evicted; a program uses a handful of ca_file values. Upgrade: LRU if that changes.
    std::map<std::tuple<bool, std::string>, std::shared_ptr<const tls_context>> tls_contexts; // loop thread only

    std::shared_ptr<const tls_context> context_for(bool verify, const std::string& ca_file, std::string& error) {
        auto key = std::make_tuple(verify, verify ? ca_file : std::string());
        auto it = tls_contexts.find(key);
        if (it != tls_contexts.end()) return it->second;
        auto c = make_tls_context(verify, ca_file, error);
        if (c) tls_contexts.emplace(std::move(key), c);
        return c;
    }

    impl() : thread([this] { io.run(); }) {}

    static key_t key_of(const async_job& j) {
        return {j.host, j.port, j.tls, j.verify, j.verify ? j.ca_file : std::string(), j.proxy_host, j.proxy_port};
    }

    std::shared_ptr<connection> take_idle(const key_t& key);
    void put_idle(const std::shared_ptr<connection>& c);
    void forget_idle(connection* c);

    // ---------------------------------------------------------------- one TCP (+TLS) connection
    // Hides resolving, connecting, the proxy CONNECT tunnel and TLS behind three operations (connect,
    // write, read) that deal in plaintext. One operation at a time; its callback runs on the loop thread
    // exactly once (also on timeout or abort). A closed connection never calls back.
    struct connection : std::enable_shared_from_this<connection> {
        impl& owner;
        key_t key;
        tcp::resolver resolver;
        tcp::socket sock;
        asio::steady_timer timer;       // deadline of the current operation
        asio::steady_timer idle_timer;  // lifetime of an idle, pooled connection
        std::uint64_t armed = 0;
        std::uint64_t watch_gen = 0;
        std::function<void(io_result)> pending;
        std::unique_ptr<tls_session> tls;
        bool tls_ready = false;
        bool reused = false;
        bool closed = false;
        long timeout = 0;
        std::string rbuf = std::string(16384, '\0');
        std::string wire;               // ciphertext (or plain bytes) being written
        std::string head;               // proxy CONNECT response head

        connection(impl& o, key_t k) : owner(o), key(std::move(k)), resolver(o.io), sock(o.io), timer(o.io), idle_timer(o.io) {}

        // ---- operation plumbing ----
        void begin(long timeout_sec, std::function<void(io_result)> cb) {
            pending = std::move(cb);
            timeout = timeout_sec;
            arm();
        }
        void arm() {
            const std::uint64_t gen = ++armed;
            if (timeout <= 0) return;
            timer.expires_after(std::chrono::seconds(timeout));
            timer.async_wait([self = shared_from_this(), gen](std::error_code ec) {
                if (ec || gen != self->armed || self->closed) return;
                self->fail(error_kind::timeout, "timed out");
            });
        }
        void finish(io_result r) {
            ++armed;
            timer.cancel();
            if (auto cb = std::move(pending)) {
                pending = nullptr;
                cb(std::move(r));
            }
        }
        void fail(error_kind k, std::string msg) {
            if (closed) return;
            close();
            io_result r;
            r.ok = false;
            r.kind = k;
            r.message = std::move(msg);
            finish(std::move(r));
        }
        void close() {
            if (closed) return;
            closed = true;
            ++watch_gen;
            std::error_code ignored;
            sock.close(ignored);
            resolver.cancel();
            idle_timer.cancel();
            owner.forget_idle(this);
        }

        // ---- connect: resolve, connect, [proxy CONNECT], [TLS handshake] ----
        void connect(const async_job& job, std::function<void(io_result)> cb) {
            const bool via_proxy = !job.proxy_host.empty();
            if (job.tls) {
                std::string err;
                auto ctx = owner.context_for(job.verify, job.ca_file, err);
                if (!ctx) { pending = std::move(cb); return fail(error_kind::tls, err); }
                tls = std::make_unique<tls_session>(std::move(ctx), job.host);
                if (!tls->ok()) { pending = std::move(cb); return fail(error_kind::tls, tls->error()); }
            }
            begin(job.timeout_sec, std::move(cb));
            const std::string& host = via_proxy ? job.proxy_host : job.host;
            const int port = via_proxy ? job.proxy_port : job.port;
            resolver.async_resolve(host, std::to_string(port),
                [self = shared_from_this(), job_host = job.host, job_port = job.port, via_proxy, tunnel = via_proxy && job.tls,
                 auth = job.proxy_authorization](std::error_code ec, tcp::resolver::results_type eps) {
                    if (self->closed) return;
                    if (ec) return self->fail(error_kind::connection, "cannot resolve host: " + ec.message());
                    self->arm();
                    asio::async_connect(self->sock, eps,
                        [self, job_host, job_port, via_proxy, tunnel, auth](std::error_code ec2, const tcp::endpoint&) {
                            if (self->closed) return;
                            if (ec2) return self->fail(error_kind::connection, (via_proxy ? "cannot connect to the proxy: " : "cannot connect: ") + ec2.message());
                            if (tunnel) return self->open_tunnel(job_host, job_port, auth);
                            self->after_tcp();
                        });
                });
        }

        void open_tunnel(const std::string& host, int port, const std::string& auth) {
            const std::string authority = host + ":" + std::to_string(port);
            std::string req = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n";
            if (!auth.empty()) req += "Proxy-Authorization: " + auth + "\r\n";
            req += "\r\n";
            wire = std::move(req);
            arm();
            asio::async_write(sock, asio::buffer(wire), [self = shared_from_this()](std::error_code ec, std::size_t) {
                if (self->closed) return;
                if (ec) return self->fail(error_kind::connection, "proxy write failed: " + ec.message());
                self->head.clear();
                self->read_tunnel_reply();
            });
        }

        void read_tunnel_reply() {
            arm();
            sock.async_read_some(asio::buffer(rbuf), [self = shared_from_this()](std::error_code ec, std::size_t n) {
                if (self->closed) return;
                if (ec) return self->fail(error_kind::connection, "the proxy closed the connection during CONNECT");
                self->head.append(self->rbuf.data(), n);
                const auto end = self->head.find("\r\n\r\n");
                if (end == std::string::npos) {
                    if (self->head.size() > 16384) return self->fail(error_kind::connection, "proxy CONNECT reply too large");
                    return self->read_tunnel_reply();
                }
                // "HTTP/1.1 200 Connection established"
                int status = 0;
                const auto sp = self->head.find(' ');
                if (sp != std::string::npos) status = std::atoi(self->head.c_str() + sp + 1);
                if (status < 200 || status > 299)
                    return self->fail(error_kind::connection, "the proxy refused CONNECT (status " + std::to_string(status) + ")" + (status == 407 ? ": proxy authentication required" : ""));
                self->head.clear();
                self->after_tcp();
            });
        }

        void after_tcp() {
            if (!tls) return finish(io_result{});
            tls_pump_handshake();
        }

        // ---- TLS handshake / read pumps share the socket I/O below ----
        void tls_pump_handshake() {
            auto r = tls->handshake();
            if (r == tls_session::result::failed) return fail(error_kind::tls, tls->error());
            if (r == tls_session::result::ok) {
                tls_ready = true;
                return flush_then([self = shared_from_this()] { self->finish(io_result{}); });
            }
            flush_then([self = shared_from_this()] {
                self->read_wire([self](const std::string& bytes) {
                    self->tls->feed(bytes.data(), bytes.size());
                    self->tls_pump_handshake();
                }, [self](bool eof, const std::string& msg) {
                    self->fail(error_kind::tls, eof ? "connection closed during the TLS handshake" : msg);
                });
            });
        }

        // Writes tls->take_output() if there is any, then calls `next`.
        void flush_then(std::function<void()> next) {
            wire = tls->take_output();
            if (wire.empty()) return next();
            arm();
            asio::async_write(sock, asio::buffer(wire), [self = shared_from_this(), next = std::move(next)](std::error_code ec, std::size_t) {
                if (self->closed) return;
                if (ec) return self->fail(self->tls_ready ? error_kind::connection : error_kind::tls, "write failed: " + ec.message());
                next();
            });
        }

        void read_wire(std::function<void(const std::string&)> on_bytes, std::function<void(bool, const std::string&)> on_error) {
            arm();
            sock.async_read_some(asio::buffer(rbuf), [self = shared_from_this(), on_bytes = std::move(on_bytes), on_error = std::move(on_error)](std::error_code ec, std::size_t n) {
                if (self->closed) return;
                if (ec) return on_error(ec == asio::error::eof, "read failed: " + ec.message());
                on_bytes(std::string(self->rbuf.data(), n));
            });
        }

        // ---- write ----
        void write(std::string data, long timeout_sec, std::function<void(io_result)> cb) {
            begin(timeout_sec, std::move(cb));
            if (!tls) {
                wire = std::move(data);
                return asio::async_write(sock, asio::buffer(wire), [self = shared_from_this()](std::error_code ec, std::size_t) {
                    if (self->closed) return;
                    if (ec) return self->fail(error_kind::connection, "write failed: " + ec.message());
                    self->finish(io_result{});
                });
            }
            if (tls->write(data) == tls_session::result::failed) return fail(error_kind::tls, tls->error());
            flush_then([self = shared_from_this()] { self->finish(io_result{}); });
        }

        // ---- read: the next chunk of plaintext, or eof ----
        void read(long timeout_sec, std::function<void(io_result)> cb) {
            begin(timeout_sec, std::move(cb));
            if (!tls) {
                return read_wire([self = shared_from_this()](const std::string& bytes) {
                    io_result r;
                    r.data = bytes;
                    self->finish(std::move(r));
                }, [self = shared_from_this()](bool eof, const std::string& msg) {
                    if (eof) { io_result r; r.eof = true; return self->finish(std::move(r)); }
                    self->fail(error_kind::connection, msg);
                });
            }
            tls_read_pump();
        }

        void tls_read_pump() {
            std::string plain;
            auto r = tls->read(plain);
            if (r == tls_session::result::ok) {
                io_result out;
                out.data = std::move(plain);
                return flush_then([self = shared_from_this(), out = std::move(out)]() mutable { self->finish(std::move(out)); });
            }
            if (r == tls_session::result::closed) { io_result e; e.eof = true; return finish(std::move(e)); }
            if (r == tls_session::result::failed) return fail(error_kind::tls, tls->error());
            flush_then([self = shared_from_this()] { // needs more ciphertext
                self->read_wire([self](const std::string& bytes) {
                    self->tls->feed(bytes.data(), bytes.size());
                    self->tls_read_pump();
                }, [self](bool eof, const std::string& msg) {
                    if (eof) { io_result e; e.eof = true; return self->finish(std::move(e)); }
                    self->fail(error_kind::connection, msg);
                });
            });
        }

        // ---- idle in the pool: notice a server that hung up ----
        void watch_idle() {
            const std::uint64_t gen = ++watch_gen;
            idle_timer.expires_after(std::chrono::seconds(idle_timeout_sec));
            idle_timer.async_wait([self = shared_from_this(), gen](std::error_code ec) {
                if (!ec && gen == self->watch_gen) self->close();
            });
            sock.async_wait(tcp::socket::wait_read, [self = shared_from_this(), gen](std::error_code ec) {
                if (ec || gen != self->watch_gen || self->closed) return;
                self->idle_readable();
            });
        }
        void stop_watch() {
            ++watch_gen;
            idle_timer.cancel();
            std::error_code ignored;
            sock.cancel(ignored);
        }
        void idle_readable() {
            if (!tls) return close(); // EOF, or bytes nobody asked for: not reusable either way
            // TLS 1.3 sends session tickets after the handshake: those are normal on an idle connection.
            // ponytail: a blocking read on a socket that is known to be readable returns at once.
            std::error_code ec;
            const std::size_t n = sock.read_some(asio::buffer(rbuf), ec);
            if (ec || n == 0) return close();
            tls->feed(rbuf.data(), n);
            std::string plain;
            if (tls->read(plain) != tls_session::result::more) return close();
            const std::string out = tls->take_output();
            if (!out.empty()) asio::write(sock, asio::buffer(out), ec);
            if (ec) return close();
            watch_idle();
        }
    };

    // ---------------------------------------------------------------- one request/response
    struct exchange : std::enable_shared_from_this<exchange> {
        impl& owner;
        async_job job;
        done_fn done;

        std::shared_ptr<connection> conn;
        std::string out;          // the request bytes

        llhttp_t parser;
        llhttp_settings_t settings;
        response res;
        bool last_was_value = true;
        bool finished = false;
        bool message_done = false;
        bool informational = false; // current message is a 1xx we skip
        bool resp_keep = false;     // the response allows the connection to be reused
        bool got_bytes = false;     // any response byte arrived on this attempt
        bool retried = false;
        std::string leftover;       // bytes after the end of the response (then the connection is not reusable)

        exchange(impl& o, async_job j, done_fn d) : owner(o), job(std::move(j)), done(std::move(d)) {
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
                e->resp_keep = llhttp_should_keep_alive(p) != 0;
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
                return HPE_PAUSED; // stop here: whatever follows is not part of this response
            };
            llhttp_init(&parser, HTTP_RESPONSE, &settings);
            parser.data = this;
        }

        void abort_now(error_kind k, const std::string& msg) { fail(k, msg); }

        // Ends the exchange (once) with an error. Never reuses the connection.
        void fail(error_kind k, const std::string& msg) {
            if (finished) return;
            HTTPP_LOG(debug) << job.method << ' ' << job.host << ':' << job.port << job.target << " failed: " << msg;
            if (conn) conn->close();
            res.error = k;
            res.error_message = msg;
            if (k != error_kind::too_large) res.status = 0;
            complete();
        }
        void succeed() {
            if (finished) return;
            if (conn) {
                if (job.keep_alive && resp_keep && leftover.empty() && !conn->closed) owner.put_idle(conn);
                else conn->close();
            }
            complete();
        }
        void complete() {
            finished = true;
            auto self = shared_from_this();
            owner.live.erase(self);
            if (done) { auto d = std::move(done); d(std::move(res)); }
        }

        void start() {
            if (!clean(job.method, false) || !clean(job.target, false) || !clean(job.host, false) ||
                job.method.empty() || job.target.empty() || !clean(job.proxy_authorization, true)) {
                return fail(error_kind::other, "invalid request line");
            }
            for (const auto& h : job.headers) {
                if (h.first.empty() || !clean(h.first, false) || !clean(h.second, true) ||
                    h.first.find(':') != std::string::npos) {
                    return fail(error_kind::other, "invalid header");
                }
            }
            const bool plain_via_proxy = !job.proxy_host.empty() && !job.tls;
            std::string target = job.target;
            if (plain_via_proxy) { // absolute form: the proxy needs to know where to go
                target = "http://" + job.host + (job.port != 80 ? ":" + std::to_string(job.port) : std::string()) + job.target;
            }
            out = job.method + " " + target + " HTTP/1.1\r\n";
            if (!has_header(job, "Host")) {
                out += "Host: " + job.host;
                if (job.port != (job.tls ? 443 : 80)) out += ":" + std::to_string(job.port);
                out += "\r\n";
            }
            for (const auto& h : job.headers) out += h.first + ": " + h.second + "\r\n";
            if (plain_via_proxy && !job.proxy_authorization.empty()) out += "Proxy-Authorization: " + job.proxy_authorization + "\r\n";
            if (!job.keep_alive && !has_header(job, "Connection")) out += "Connection: close\r\n";
            bool wants_len = !job.body.empty() || ieq(job.method, "POST") || ieq(job.method, "PUT") ||
                             ieq(job.method, "PATCH");
            if (wants_len && !has_header(job, "Content-Length"))
                out += "Content-Length: " + std::to_string(job.body.size()) + "\r\n";
            out += "\r\n";
            out += job.body;
            use_connection(false);
        }

        void use_connection(bool fresh) {
            const key_t key = impl::key_of(job);
            if (!fresh && job.keep_alive) {
                if (auto c = owner.take_idle(key)) {
                    conn = std::move(c);
                    conn->reused = true;
                    return send();
                }
            }
            conn = std::make_shared<connection>(owner, key);
            conn->connect(job, [self = shared_from_this()](io_result r) {
                if (self->finished) return;
                if (!r.ok) return self->fail(r.kind, r.message);
                self->send();
            });
        }

        // The connection was reused and the server had already hung up: try once more on a new one.
        bool maybe_retry(const io_result& r) {
            if (!conn || !conn->reused || got_bytes || retried || r.kind == error_kind::timeout || !idempotent(job.method)) return false;
            retried = true;
            conn->close();
            llhttp_reset(&parser);
            res = response{};
            last_was_value = true;
            message_done = false;
            informational = false;
            leftover.clear();
            use_connection(true);
            return true;
        }

        void send() {
            conn->write(out, job.timeout_sec, [self = shared_from_this()](io_result r) {
                if (self->finished) return;
                if (!r.ok) {
                    if (self->maybe_retry(r)) return;
                    return self->fail(r.kind, r.message);
                }
                self->read();
            });
        }

        void read() {
            conn->read(job.timeout_sec, [self = shared_from_this()](io_result r) {
                if (self->finished) return;
                if (!r.ok) {
                    if (self->maybe_retry(r)) return;
                    return self->fail(r.kind, r.message);
                }
                if (r.eof) {
                    if (!self->got_bytes && self->maybe_retry(r)) return;
                    return self->on_eof();
                }
                self->got_bytes = true;
                if (!self->feed_parser(r.data.data(), r.data.size())) return;
                self->read();
            });
        }

        // Plaintext HTTP bytes into llhttp. Returns false once the exchange is over (done or failed).
        bool feed_parser(const char* data, std::size_t n) {
            llhttp_errno_t r = llhttp_execute(&parser, data, n);
            if (finished) return false; // too_large already reported from the body callback
            if (r == HPE_PAUSED) { // the response is complete; anything after it is stray
                const char* pos = llhttp_get_error_pos(&parser);
                if (pos && pos >= data && pos < data + n) leftover.assign(pos, static_cast<std::size_t>(data + n - pos));
                succeed();
                return false;
            }
            if (r != HPE_OK && r != HPE_PAUSED_UPGRADE) {
                fail(error_kind::other, std::string("malformed response: ") + llhttp_errno_name(r));
                return false;
            }
            return true;
        }

        // The peer closed the connection: a body that ends there is legal, a cut-off one is not.
        void on_eof() {
            llhttp_errno_t r = llhttp_finish(&parser);
            if ((r == HPE_OK || r == HPE_PAUSED) && (message_done || res.status != 0)) {
                resp_keep = false; // it ended by closing: the connection is gone
                return succeed();
            }
            fail(error_kind::connection, "connection closed before the response was complete");
        }
    };
};

std::shared_ptr<async_http::impl::connection> async_http::impl::take_idle(const key_t& key) {
    auto it = idle.find(key);
    if (it == idle.end()) return nullptr;
    while (!it->second.empty()) {
        auto c = std::move(it->second.back());
        it->second.pop_back();
        if (!c->closed) {
            c->stop_watch();
            if (it->second.empty()) idle.erase(it);
            return c;
        }
    }
    idle.erase(it);
    return nullptr;
}

void async_http::impl::put_idle(const std::shared_ptr<connection>& c) {
    auto& list = idle[c->key];
    if (list.size() >= max_idle_per_origin) return c->close();
    list.push_back(c);
    c->watch_idle();
}

void async_http::impl::forget_idle(connection* c) {
    auto it = idle.find(c->key);
    if (it == idle.end()) return;
    auto& list = it->second;
    list.erase(std::remove_if(list.begin(), list.end(), [c](const std::shared_ptr<connection>& p) { return p.get() == c; }), list.end());
    if (list.empty()) idle.erase(it);
}

async_http::async_http() : impl_(std::make_unique<impl>()) {}

async_http::~async_http() {
    // Cancel what is in flight on the loop thread, then let the loop drain and exit.
    asio::post(impl_->io, [this] {
        auto copy = impl_->live;
        for (auto& e : copy) e->abort_now(error_kind::canceled, "client destroyed");
        std::vector<std::shared_ptr<impl::connection>> pooled;
        for (auto& [key, list] : impl_->idle) for (auto& c : list) pooled.push_back(c);
        for (auto& c : pooled) c->close();
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
