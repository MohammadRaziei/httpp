#include "internal/async_server.hpp"
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

#include "llhttp.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <unordered_set>

namespace httpp::detail {
namespace {

using asio::ip::tcp;

constexpr std::size_t max_uri = 8192;          // request target
constexpr std::size_t max_header_line = 8192;  // one header name or value
constexpr std::size_t max_header_total = 65536; // the whole head
constexpr int linger_seconds = 2;               // how long to drain a request we refused

const char* reason(int status) {
    switch (status) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return status >= 200 && status < 300 ? "OK" : status >= 400 && status < 500 ? "Client Error"
                       : status >= 500 ? "Server Error" : "Unknown";
    }
}

bool ieq(const std::string& a, const char* b) {
    const std::size_t n = std::char_traits<char>::length(b);
    return a.size() == n && std::equal(a.begin(), a.end(), b, [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}

// "Sat, 10 Oct 2026 12:00:00 GMT"; locale independent; cached per second (loop thread only).
const std::string& http_date() {
    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static std::time_t cached = 0;
    static std::string text;
    const std::time_t now = std::time(nullptr);
    if (now != cached || text.empty()) {
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &now);
#else
        gmtime_r(&now, &tm);
#endif
        char buf[48];
        std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT", days[tm.tm_wday], tm.tm_mday,
                      months[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
        text = buf;
        cached = now;
    }
    return text;
}

// Answers may come from any thread; this lets them post to the loop without touching a loop that
// was destroyed in the meantime (io becomes null in ~impl).
struct poster {
    std::mutex mu;
    asio::io_context* io = nullptr;
    void post(std::function<void()> fn) {
        std::lock_guard<std::mutex> lk(mu);
        if (io) asio::post(*io, std::move(fn));
    }
};

} // namespace

struct async_server::impl {
    std::atomic<int> read_to, write_to, ka_to;
    std::atomic<int> head_to{10};          // seconds for a whole request head to arrive (slowloris), 0 = no limit
    std::atomic<int> body_to{120};         // seconds for a whole request body to arrive after its head, 0 = no limit
    std::atomic<std::size_t> ka_max, max_body;
    std::atomic<std::size_t> max_conn{0};  // 0 = no limit
    request_fn on_request;
    blocking_fn run_blocking;

    asio::io_context io;
    tcp::acceptor acceptor{io};
    asio::steady_timer backoff{io};
    std::shared_ptr<poster> poster_ = std::make_shared<poster>();
    std::unordered_set<std::shared_ptr<asio::steady_timer>> timers; // loop thread only
    bool closing = false;                                           // loop thread only
    bool failed = false;

    std::mutex mu; // lifecycle flags
    bool bound = false, running = false, stop_requested = false;
    bool reuse_port = false;

    struct conn;
    std::unordered_set<std::shared_ptr<conn>> conns; // loop thread only

    impl(options o, request_fn r, blocking_fn b)
        : read_to(o.read_timeout_sec), write_to(o.write_timeout_sec), ka_to(o.keep_alive_timeout_sec),
          ka_max(o.keep_alive_max == 0 ? 1 : o.keep_alive_max), max_body(o.max_body),
          on_request(std::move(r)), run_blocking(std::move(b)) {
        poster_->io = &io;
    }
    ~impl() {
        std::lock_guard<std::mutex> lk(poster_->mu);
        poster_->io = nullptr;
    }

    void begin_close();
    void accept_next();
};

// One client connection. Lives on the loop thread; answers and body pieces hop back onto it.
struct async_server::impl::conn : std::enable_shared_from_this<conn> {
    impl& srv;
    tcp::socket sock;
    asio::steady_timer timer;
    llhttp_t parser;
    llhttp_settings_t settings;

    std::string rbuf = std::string(16384, '\0');
    std::string leftover;     // bytes of a pipelined next request, parsed after this one is answered
    std::string wbuf;         // what is being written
    http_request req;         // being assembled
    bool last_was_value = true;
    bool headers_done = false;
    std::size_t header_bytes = 0;
    int reject = 0;           // set by a parser callback that wants the request refused with this status
    bool got_bytes = false;   // part of a request has arrived (read timeout instead of idle timeout)
    std::chrono::steady_clock::time_point head_start; // when the first byte of this request arrived
    std::chrono::steady_clock::time_point body_start; // when the head of this request was complete
    bool req_keep = false;    // the client allows keep-alive
    bool req_head = false;
    int req_minor = 1;
    std::string log_what;     // "METHOD target" of the request being answered, kept only when the info level is on
    std::size_t remaining;    // requests this connection may still serve, counting the current one

    std::atomic<bool> closed{false};
    std::uint64_t armed = 0;
    bool answering = false;   // a request was handed out and its answer has not been written yet

    struct stream_state {
        std::function<bool(std::string&)> next;
        long long length = -1;
        long long sent = 0;
        bool chunked = false;
        bool keep = false;
    };
    std::shared_ptr<stream_state> stream;
    // Holds the answer's after_send job. Dropping it (the answer is written, or the connection closed) hands the job to
    // the pool, so the job runs exactly once whichever way the answer ends.
    std::shared_ptr<void> sent_guard;

    conn(impl& s, tcp::socket sk) : srv(s), sock(std::move(sk)), timer(s.io), remaining(s.ka_max.load()) {
        llhttp_settings_init(&settings);
        settings.on_url = [](llhttp_t* p, const char* at, std::size_t n) -> int {
            auto* c = static_cast<conn*>(p->data);
            c->req.target.append(at, n);
            if (c->req.target.size() > max_uri) { c->reject = 414; return HPE_USER; }
            return 0;
        };
        settings.on_header_field = [](llhttp_t* p, const char* at, std::size_t n) -> int {
            auto* c = static_cast<conn*>(p->data);
            if (c->last_was_value) c->req.headers.emplace_back();
            c->last_was_value = false;
            c->header_bytes += n + 4;
            auto& name = c->req.headers.back().first;
            name.append(at, n);
            if (name.size() > max_header_line || c->header_bytes > max_header_total) { c->reject = 431; return HPE_USER; }
            return 0;
        };
        settings.on_header_value = [](llhttp_t* p, const char* at, std::size_t n) -> int {
            auto* c = static_cast<conn*>(p->data);
            c->last_was_value = true;
            c->header_bytes += n;
            auto& value = c->req.headers.back().second;
            value.append(at, n);
            if (value.size() > max_header_line || c->header_bytes > max_header_total) { c->reject = 431; return HPE_USER; }
            return 0;
        };
        settings.on_headers_complete = [](llhttp_t* p) -> int {
            auto* c = static_cast<conn*>(p->data);
            c->headers_done = true;
            c->body_start = std::chrono::steady_clock::now();
            c->req.method = llhttp_method_name(static_cast<llhttp_method_t>(p->method));
            c->req.version_minor = p->http_minor;
            c->req_minor = p->http_minor;
            c->req_head = c->req.method == "HEAD";
            c->req_keep = llhttp_should_keep_alive(p) != 0;
            const std::size_t limit = c->srv.max_body.load();
            if (limit && p->content_length > limit && !(p->flags & F_CHUNKED)) { c->reject = 413; return HPE_USER; }
            if (p->http_minor >= 1) {
                for (const auto& h : c->req.headers) {
                    if (ieq(h.first, "Expect") && ieq(h.second, "100-continue")) {
                        // ponytail: 25 bytes into an empty send buffer, so a blocking write cannot stall.
                        // Upgrade: make it an async write if this ever shows up in a profile.
                        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
                        std::error_code ec;
                        asio::write(c->sock, asio::buffer(cont, sizeof cont - 1), ec);
                    }
                }
            }
            return 0;
        };
        settings.on_body = [](llhttp_t* p, const char* at, std::size_t n) -> int {
            auto* c = static_cast<conn*>(p->data);
            const std::size_t limit = c->srv.max_body.load();
            if (limit && c->req.body.size() + n > limit) { c->reject = 413; return HPE_USER; }
            c->req.body.append(at, n);
            return 0;
        };
        settings.on_message_complete = [](llhttp_t*) -> int { return HPE_PAUSED; }; // one request at a time
        llhttp_init(&parser, HTTP_REQUEST, &settings);
        parser.data = this;
    }

    // ---- timers ----
    void arm(int seconds) {
        const std::uint64_t gen = ++armed;
        if (seconds <= 0) { timer.cancel(); return; }
        timer.expires_after(std::chrono::seconds(seconds));
        timer.async_wait([self = shared_from_this(), gen](std::error_code ec) {
            if (!ec && gen == self->armed) self->close();
        });
    }
    void arm_ms(long long ms) {
        const std::uint64_t gen = ++armed;
        timer.expires_after(std::chrono::milliseconds(ms));
        timer.async_wait([self = shared_from_this(), gen](std::error_code ec) {
            if (!ec && gen == self->armed) self->close();
        });
    }
    void disarm() { ++armed; timer.cancel(); }

    void close() {
        if (closed.exchange(true)) return;
        HTTPP_LOG(trace) << "connection closed";
        auto self = shared_from_this();
        ++armed;
        std::error_code ec;
        timer.cancel();
        sock.close(ec);
        stream.reset(); // releases a body generator whose client went away
        sent_guard.reset();
        srv.conns.erase(self);
    }

    // ---- reading ----
    void start() { next_request(); }

    void next_request() {
        if (closed) return;
        req = http_request{};
        last_was_value = true;
        headers_done = false;
        header_bytes = 0;
        reject = 0;
        answering = false;
        llhttp_reset(&parser);
        if (!leftover.empty()) {
            std::string d = std::move(leftover);
            leftover.clear();
            got_bytes = true;
            head_start = std::chrono::steady_clock::now();
            feed(d.data(), d.size());
        } else {
            got_bytes = false;
            read();
        }
    }

    void read() {
        const int per_read = got_bytes ? srv.read_to.load() : srv.ka_to.load();
        // A client that sends a byte now and then keeps every single read alive: the whole head and
        // the whole body each have one deadline too (slowloris).
        const bool in_head = !headers_done;
        const int total = in_head ? srv.head_to.load() : srv.body_to.load();
        if (got_bytes && total > 0) {
            const auto since = in_head ? head_start : body_start;
            const long long left = total * 1000LL - std::chrono::duration_cast<std::chrono::milliseconds>(
                                                        std::chrono::steady_clock::now() - since).count();
            if (left <= 0) {
                HTTPP_LOG(debug) << "request " << (in_head ? "head" : "body") << " took longer than " << total
                                 << " s: closing";
                return close();
            }
            arm_ms(per_read > 0 ? std::min<long long>(left, per_read * 1000LL) : left);
        } else {
            arm(per_read);
        }
        sock.async_read_some(asio::buffer(rbuf), [self = shared_from_this()](std::error_code ec, std::size_t n) {
            if (self->closed) return;
            if (ec) return self->close(); // peer closed, or timeout/shutdown cancelled the read
            if (!self->got_bytes) self->head_start = std::chrono::steady_clock::now();
            self->got_bytes = true;
            self->feed(self->rbuf.data(), n);
        });
    }

    void feed(const char* data, std::size_t n) {
        const llhttp_errno_t err = llhttp_execute(&parser, data, n);
        if (closed) return;
        switch (err) {
            case HPE_OK:
                return read(); // need more bytes
            case HPE_PAUSED:
            case HPE_PAUSED_UPGRADE: {
                const char* pos = llhttp_get_error_pos(&parser);
                if (pos && pos >= data && pos <= data + n) leftover.assign(pos, static_cast<std::size_t>(data + n - pos));
                if (err == HPE_PAUSED_UPGRADE) llhttp_resume_after_upgrade(&parser);
                else llhttp_resume(&parser);
                return request_complete();
            }
            default:
                return refuse(reject ? reject : 400);
        }
    }

    void request_complete() {
        disarm();
        answering = true;
        if (detail::log_enabled(log_level::info)) log_what = req.method + ' ' + req.target;  // req is moved below
        auto once = std::make_shared<std::atomic<bool>>(false);
        auto post = srv.poster_;
        std::weak_ptr<conn> weak = shared_from_this();
        answer_fn answer = [weak, once, post](wire_response r) {
            if (once->exchange(true)) return;
            post->post([weak, r = std::move(r)]() mutable {
                if (auto c = weak.lock()) c->send(std::move(r));
            });
        };
        if (!srv.on_request(std::move(req), std::move(answer))) close();
    }

    // The engine's own error answers (400, 413, 414, 431): always close afterwards.
    void refuse(int status) {
        HTTPP_LOG(debug) << "refusing a request with " << status << " " << reason(status);
        disarm();
        answering = true;
        if (detail::log_enabled(log_level::info)) log_what = req.method + ' ' + req.target;
        wire_response r;
        r.status = status;
        r.close = true;
        r.headers.emplace_back("Content-Type", "text/plain");
        r.body = reason(status);
        send(std::move(r));
    }

    // ---- writing ----
    void write(std::string data, std::function<void()> then) {
        wbuf = std::move(data);
        arm(srv.write_to.load());
        asio::async_write(sock, asio::buffer(wbuf),
                          [self = shared_from_this(), then = std::move(then)](std::error_code ec, std::size_t) {
                              if (self->closed) return;
                              if (ec) return self->close();
                              self->disarm();
                              then();
                          });
    }

    void send(wire_response r) {
        if (closed || !answering) return;
        answering = false;
        if (r.status == 0) return close(); // convention: no answer at all (a handler that gave up)
        if (r.after_send) sent_guard = make_sent_guard(std::move(r.after_send));
        if (!log_what.empty()) {  // the access log
            HTTPP_LOG(info) << log_what << ' ' << r.status;
            log_what.clear();
        }
        const bool bodyless = req_head || r.status < 200 || r.status == 204 || r.status == 304;
        bool keep = req_keep && remaining > 1 && !r.close && !srv.closing;
        const bool streaming = static_cast<bool>(r.next);
        bool chunked = false;
        if (streaming && r.length < 0 && !bodyless) {
            if (req_minor >= 1) chunked = true;
            else keep = false; // HTTP/1.0 cannot do chunked: the end of the body is the end of the connection
        }

        std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
        for (const auto& h : r.headers) {
            if (ieq(h.first, "Content-Length") || ieq(h.first, "Transfer-Encoding") || ieq(h.first, "Connection") ||
                ieq(h.first, "Date"))
                continue; // the engine owns these
            out += h.first + ": " + h.second + "\r\n";
        }
        out += "Date: " + http_date() + "\r\n";
        if (!(r.status < 200 || r.status == 204)) {
            if (chunked) out += "Transfer-Encoding: chunked\r\n";
            else if (!streaming) out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
            else if (r.length >= 0) out += "Content-Length: " + std::to_string(r.length) + "\r\n";
        }
        if (!keep) out += "Connection: close\r\n";
        else if (req_minor == 0) out += "Connection: keep-alive\r\n";
        out += "\r\n";

        if (!streaming || bodyless) {
            if (!bodyless && !streaming) out += r.body;
            return write(std::move(out), [self = shared_from_this(), keep] { self->done(keep); });
        }
        stream = std::make_shared<stream_state>();
        stream->next = std::move(r.next);
        stream->length = r.length;
        stream->chunked = chunked;
        stream->keep = keep;
        write(std::move(out), [self = shared_from_this()] { self->pump(); });
    }

    // Asks a pool thread for the next piece of a streamed body, writes it, repeats.
    void pump() {
        auto st = stream;
        if (!st || closed) return;
        auto self = shared_from_this();
        auto post = srv.poster_;
        const bool queued = srv.run_blocking([self, st, post] {
            std::string piece;
            bool more = false, failed = false;
            try {
                more = st->next(piece);
            } catch (...) {
                failed = true;
            }
            post->post([self, st, piece = std::move(piece), more, failed]() mutable {
                self->piece_ready(st, std::move(piece), more, failed);
            });
        });
        if (!queued) close();
    }

    void piece_ready(const std::shared_ptr<stream_state>& st, std::string piece, bool more, bool failed) {
        if (closed || stream != st) return;
        if (failed) {
            HTTPP_LOG(warning) << "a response body generator threw: dropping the connection";
            return close(); // never end a broken stream cleanly: the client must see it fail
        }
        st->sent += static_cast<long long>(piece.size());
        if (st->length >= 0 && (st->sent > st->length || (!more && st->sent != st->length))) {
            HTTPP_LOG(warning) << "a response body produced " << st->sent << " bytes but announced " << st->length << ": dropping the connection";
            return close();
        }
        std::string out;
        if (!piece.empty()) {
            if (st->chunked) {
                char hex[24];
                std::snprintf(hex, sizeof hex, "%zx\r\n", piece.size());
                out += hex;
                out += piece;
                out += "\r\n";
            } else {
                out = std::move(piece);
            }
        }
        if (!more) {
            if (st->chunked) out += "0\r\n\r\n";
            const bool keep = st->keep;
            stream.reset(); // the generator is released as soon as the body is complete
            if (out.empty()) return done(keep);
            return write(std::move(out), [self = shared_from_this(), keep] { self->done(keep); });
        }
        if (out.empty()) return pump();
        write(std::move(out), [self = shared_from_this()] { self->pump(); });
    }

    std::shared_ptr<void> make_sent_guard(std::function<void()> job) {
        // The deleter runs on the loop thread: it only enqueues, the job itself runs on a pool thread.
        return std::shared_ptr<void>(static_cast<void*>(this), [job = std::move(job), run = srv.run_blocking, post = srv.poster_](void*) {
            {
                std::lock_guard<std::mutex> lk(post->mu);
                if (!post->io) return; // the server is being destroyed: its pool may be gone, drop the job
            }
            run([job] {
                try {
                    job();
                } catch (...) {
                    HTTPP_LOG(warning) << "an after_send job threw: ignored";
                }
            });
        });
    }

    void done(bool keep) {
        sent_guard.reset();
        if (closed) return;
        if (!keep) return linger();
        --remaining;
        next_request();
    }

    // Closing right after a response can reset the connection and destroy the answer before the
    // client has read it (when request bytes are still in flight, as with a refused upload). So: stop
    // sending, read and discard whatever arrives until the peer is done (or a short limit), then close.
    void linger() {
        std::error_code ec;
        sock.shutdown(tcp::socket::shutdown_send, ec);
        arm(linger_seconds);
        drain();
    }
    void drain() {
        sock.async_read_some(asio::buffer(rbuf), [self = shared_from_this()](std::error_code ec, std::size_t) {
            if (self->closed) return;
            if (ec) return self->close();
            self->drain();
        });
    }
};

void async_server::impl::begin_close() {
    if (closing) return;
    closing = true;
    std::error_code ec;
    acceptor.close(ec);
    backoff.cancel();
    for (auto& t : timers) t->cancel();
    timers.clear();
    auto all = conns; // close() erases from the set
    for (auto& c : all) c->close();
}

void async_server::impl::accept_next() {
    acceptor.async_accept([this](std::error_code ec, tcp::socket s) {
        if (closing) return; // `s` closes itself
        if (!ec) {
            std::error_code ignore;
            const std::size_t cap = max_conn.load();
            if (cap && conns.size() >= cap) { // over the limit: a short 503, then hang up
                static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
                s.send(asio::buffer(busy, sizeof busy - 1), 0, ignore); // best effort into an empty send buffer
                HTTPP_LOG(debug) << "connection refused: " << cap << " connections already open";
                s.close(ignore);
                accept_next();
                return;
            }
            s.set_option(tcp::no_delay(true), ignore);
            auto c = std::make_shared<conn>(*this, std::move(s));
            conns.insert(c);
            c->start();
            accept_next();
            return;
        }
        if (ec == asio::error::operation_aborted) return;
        if (!acceptor.is_open()) { // the listener is gone: nothing left to accept on
            HTTPP_LOG(error) << "listener failed: " << ec.message();
            failed = true;
            begin_close();
            return;
        }
        // Transient (aborted handshake) or resource exhaustion (out of fds): pause briefly, keep accepting.
        const bool exhausted = ec == asio::error::no_descriptors || ec == asio::error::no_buffer_space ||
                               ec == asio::error::no_memory;
        if (exhausted) { HTTPP_LOG(warning) << "accept failed (" << ec.message() << "): out of resources, retrying"; } // braces: the macro ends in an else
        backoff.expires_after(std::chrono::milliseconds(exhausted ? 20 : 1));
        backoff.async_wait([this](std::error_code e) {
            if (!e && !closing) accept_next();
        });
    });
}

async_server::async_server(options opt, request_fn on_request, blocking_fn run_blocking)
    : impl_(std::make_unique<impl>(opt, std::move(on_request), std::move(run_blocking))) {}

async_server::~async_server() = default;

void async_server::set_read_timeout(int seconds) { impl_->read_to = seconds; }
void async_server::set_write_timeout(int seconds) { impl_->write_to = seconds; }
void async_server::set_keep_alive_timeout(int seconds) { impl_->ka_to = seconds; }
void async_server::set_keep_alive_max(std::size_t requests) { impl_->ka_max = requests == 0 ? 1 : requests; }
void async_server::set_max_body(std::size_t bytes) { impl_->max_body = bytes; }
void async_server::set_header_timeout(int seconds) { impl_->head_to = seconds; }
void async_server::set_body_timeout(int seconds) { impl_->body_to = seconds; }
void async_server::set_max_connections(std::size_t n) { impl_->max_conn = n; }
void async_server::set_reuse_port(bool on) { impl_->reuse_port = on; }

void async_server::after(std::chrono::milliseconds delay, std::function<void()> fn) {
    auto& m = *impl_;
    std::lock_guard<std::mutex> lock(m.mu);
    if (!m.running) return;
    asio::post(m.io, [&m, delay, fn = std::move(fn)]() mutable {
        if (m.closing) return;
        auto timer = std::make_shared<asio::steady_timer>(m.io);
        m.timers.insert(timer);
        timer->expires_after(delay);
        timer->async_wait([&m, timer, fn = std::move(fn)](std::error_code ec) {
            m.timers.erase(timer);
            if (!ec) fn();
        });
    });
}

int async_server::bind(const std::string& host, int port) {
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
    // SO_REUSEADDR on Windows lets a second process take over a port that is already served; ask
    // for the opposite instead.
    const int one = 1;
    ::setsockopt(m.acceptor.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one),
                 sizeof(one));
#else
    // Fast restart after a stop (TIME_WAIT), but a second server cannot bind a port that is already listened on.
    m.acceptor.set_option(asio::socket_base::reuse_address(true), ec);
#endif
    if (m.reuse_port) {
        // Not SO_REUSEADDR: on Windows that would let anyone take over the port. Only Linux spreads
        // connections between the listeners fairly, so that is the one platform we claim.
#if defined(SO_REUSEPORT) && !defined(_WIN32)
        const int on = 1;
        if (::setsockopt(m.acceptor.native_handle(), SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) != 0) {
            m.acceptor.close(ec);
            return -1;
        }
#else
        m.acceptor.close(ec);
        return -1;
#endif
    }
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

bool async_server::run() {
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
    if (stop_now) asio::post(m.io, [&m] { m.begin_close(); });

    m.io.run(); // returns once the listener, timers and connections are all closed

    {
        std::lock_guard<std::mutex> lock(m.mu);
        m.running = false;
        m.bound = false;
    }
    // Run what was queued meanwhile (late answers); their connections are closed, so they do nothing.
    m.io.restart();
    m.io.poll();
    return !m.failed;
}

void async_server::stop() {
    auto& m = *impl_;
    std::lock_guard<std::mutex> lock(m.mu);
    if (!m.bound) return;
    m.stop_requested = true;
    if (m.running) asio::post(m.io, [&m] { m.begin_close(); });
}

} // namespace httpp::detail
