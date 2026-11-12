#include "httpp/server.hpp"

// httpp::server on top of the async engine (internal/async_server.hpp: Asio + llhttp). Routing,
// static files, hooks and response building live here; the engine only moves bytes.

#include "internal/async_server.hpp"
#include "internal/log.hpp"
#include "internal/response_compression.hpp"
#include "internal/urlencoded.hpp"
#include "internal/websocket.hpp"
#include "internal/worker_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace httpp {
namespace {

// ---------------------------------------------------------------- small helpers

inline unsigned char ascii_lower(unsigned char c) { return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + 32) : c; }

// Same bytes, ASCII case folded. The length test is inline, so for a literal it is a constant compare (no strlen call).
bool ieq_bytes(const char* a, const char* b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (ascii_lower(static_cast<unsigned char>(a[i])) != ascii_lower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
inline bool ieq(const std::string& a, const char* b) {
    const std::size_t n = std::char_traits<char>::length(b);
    return a.size() == n && ieq_bytes(a.data(), b, n);
}

using detail::parse_query;
using detail::percent_decode;

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t pos = 1; // skip the leading '/'
    while (pos <= path.size()) {
        std::size_t end = path.find('/', pos);
        if (end == std::string::npos) end = path.size();
        parts.push_back(path.substr(pos, end - pos));
        pos = end + 1;
    }
    return parts;
}

bool header_ok(std::string_view name, std::string_view value) { // views: a literal name costs no std::string
    if (name.empty()) return false;
    for (unsigned char c : name) if (c <= ' ' || c == ':' || c == 0x7f) return false;
    for (unsigned char c : value) if (c == '\r' || c == '\n' || c == 0) return false;
    return true;
}

const char* mime_for(const std::string& path) {
    static const struct { const char* ext; const char* type; } table[] = {
        {"html", "text/html"}, {"htm", "text/html"}, {"css", "text/css"}, {"js", "text/javascript"},
        {"mjs", "text/javascript"}, {"json", "application/json"}, {"xml", "application/xml"},
        {"txt", "text/plain"}, {"md", "text/markdown"}, {"csv", "text/csv"}, {"png", "image/png"},
        {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"}, {"svg", "image/svg+xml"},
        {"ico", "image/x-icon"}, {"webp", "image/webp"}, {"avif", "image/avif"}, {"pdf", "application/pdf"},
        {"zip", "application/zip"}, {"gz", "application/gzip"}, {"tar", "application/x-tar"},
        {"wasm", "application/wasm"}, {"woff", "font/woff"}, {"woff2", "font/woff2"}, {"ttf", "font/ttf"},
        {"otf", "font/otf"}, {"mp3", "audio/mpeg"}, {"wav", "audio/wav"}, {"ogg", "audio/ogg"},
        {"mp4", "video/mp4"}, {"webm", "video/webm"}, {"yaml", "application/yaml"}, {"yml", "application/yaml"},
    };
    const std::size_t dot = path.rfind('.');
    const std::size_t slash = path.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        std::string ext = path.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        for (const auto& e : table) if (ext == e.ext) return e.type;
    }
    return "application/octet-stream";
}

// Size of a readable regular file, or -1. (A file that exists but cannot be opened is -1 too.)
long long file_size(const std::string& file) {
    const httpp::path p(file);
    const httpp::path::status i = p.stat();
    if (i.type != httpp::path::kind::file) return -1;
    if (!std::ifstream(p.native(), std::ios::binary)) return -1;
    return i.size;
}

// ---- validators of a file answer (ADR-0045)

// Strong validator from the size and the modification time (nanoseconds where known): it changes whenever either does.
std::string make_etag(long long size, const httpp::path::status& st) {
    char buf[80];
    std::snprintf(buf, sizeof buf, "\"%llx-%llx-%lx\"", static_cast<unsigned long long>(size), static_cast<unsigned long long>(st.mtime),
                  static_cast<unsigned long>(st.mtime_nsec));
    return buf;
}

const char* const k_days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const k_months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string format_http_date(long long t) {
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT", k_days[tm.tm_wday], tm.tm_mday, k_months[tm.tm_mon], tm.tm_year + 1900,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm); no timegm, which Windows lacks.
long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

// Only the IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT"), which is what every current client sends; the two obsolete formats are ignored,
// and a header that is ignored behaves as if it was not sent (RFC 9110 section 13.1.3).
bool parse_http_date(const std::string& s, long long& out) {
    if (s.size() != 29 || s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' || s[19] != ':' || s[22] != ':' || s[25] != ' ' ||
        s.compare(26, 3, "GMT") != 0)
        return false;
    bool day_ok = false;
    for (const char* d : k_days) if (s.compare(0, 3, d) == 0) day_ok = true;
    if (!day_ok) return false;
    unsigned month = 0;
    for (unsigned i = 0; i < 12; ++i) if (s.compare(8, 3, k_months[i]) == 0) month = i + 1;
    if (month == 0) return false;
    auto num = [&](std::size_t at, std::size_t n, long long& v) {
        v = 0;
        for (std::size_t i = at; i < at + n; ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
            v = v * 10 + (s[i] - '0');
        }
        return true;
    };
    long long day, year, hour, minute, second;
    if (!num(5, 2, day) || !num(12, 4, year) || !num(17, 2, hour) || !num(20, 2, minute) || !num(23, 2, second)) return false;
    if (day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return false;
    out = days_from_civil(year, month, static_cast<unsigned>(day)) * 86400 + hour * 3600 + minute * 60 + second;
    return true;
}

// Does the comma separated list of entity tags (If-Match, If-None-Match) hold `etag`? "*" matches anything. A weak tag ("W/...") is skipped when
// `strong_only` (If-Match), and otherwise compared by its opaque part (If-None-Match). ponytail: a comma inside a tag splits it; our tags have none.
bool etag_list_has(const std::string& list, const std::string& etag, bool strong_only) {
    std::size_t pos = 0;
    while (pos <= list.size()) {
        std::size_t end = list.find(',', pos);
        if (end == std::string::npos) end = list.size();
        std::size_t a = pos, b = end;
        while (a < b && (list[a] == ' ' || list[a] == '\t')) ++a;
        while (b > a && (list[b - 1] == ' ' || list[b - 1] == '\t')) --b;
        std::string token = list.substr(a, b - a);
        pos = end + 1;
        if (token == "*") return true;
        if (token.compare(0, 2, "W/") == 0) {
            if (strong_only) continue;
            token.erase(0, 2);
        }
        if (token == etag) return true;
    }
    return false;
}

enum class precondition { proceed, not_modified, failed };

// RFC 9110 section 13.2.2 for a GET or HEAD of a file, in the order the RFC gives. `honour_range` is false when an If-Range
// says the client's copy is not this file: the Range header is then ignored and the whole file is sent.
precondition evaluate_preconditions(const server_request& req, const std::string& etag, long long mtime, bool& honour_range) {
    honour_range = true;
    long long when = 0;
    const std::string if_match = req.header("If-Match");
    if (!if_match.empty()) {
        if (!etag_list_has(if_match, etag, true)) return precondition::failed;
    } else if (parse_http_date(req.header("If-Unmodified-Since"), when) && mtime > when) {
        return precondition::failed;
    }
    const std::string if_none_match = req.header("If-None-Match");
    if (!if_none_match.empty()) {
        if (etag_list_has(if_none_match, etag, false)) return precondition::not_modified;
    } else if (parse_http_date(req.header("If-Modified-Since"), when) && mtime <= when) {
        return precondition::not_modified;
    }
    const std::string if_range = req.header("If-Range");
    if (!if_range.empty() && !req.header("Range").empty()) {
        if (if_range[0] == '"') honour_range = if_range == etag;
        else if (if_range.compare(0, 2, "W/") == 0) honour_range = false; // a weak tag never matches here
        else honour_range = parse_http_date(if_range, when) && when == mtime;
    }
    return precondition::proceed;
}

std::string describe(std::exception_ptr ep) {
    try {
        if (ep) std::rethrow_exception(ep);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
    }
    return "unknown exception";
}

// ---------------------------------------------------------------- the server

struct config { // read-only once listening; shared with answers that may outlive the server
    server::before_hook before;  // what the engine calls: the middleware and the application's hook, composed by rebuild_hooks()
    server::after_hook after;
    server::before_hook app_before; // the application's own hooks (server::before / server::after)
    server::after_hook app_after;
    std::vector<server::before_hook> mw_before; // server::use(), in registration order
    std::vector<server::after_hook> mw_after;
    server::error_hook error;
    server::exception_hook exception;
    detail::response_compression compression; // off unless set_compression() was called
    std::atomic<detail::worker_pool*> pool{nullptr}; // the running pool, or null when not listening
};

struct route_entry {
    std::string method;
    std::vector<std::string> segs; // ":name" segments capture
    handler h;          // exactly one of these four is set
    shared_handler sh;
    async_handler ah;
    io_handler ih;
    std::shared_ptr<const ws_handler> ws;  // a WebSocket route (route_ws): the handshake runs on the I/O thread like route_io
    bool on_io_thread() const { return ih || ws; }
};

// A request that is parsed (and maybe already matched to a route), on its way to be run.
struct prepared {
    std::shared_ptr<server_request> req;
    std::string raw_path;                // the path as sent, still percent-encoded
    const route_entry* route = nullptr;  // valid when `matched`; null = no route fits
    bool matched = false;
};

struct mount_entry {
    std::string prefix; // "/files" (no trailing slash, "" = root)
    std::string dir;
    server::directory_options options;
};

// True on the threads that run an engine (the I/O threads) while the server listens.
thread_local bool tl_on_io_thread = false;
// On an I/O thread: the list of waiting async requests that belongs to that thread's engine, and the server it belongs to (see server::impl::waiting).
thread_local void* tl_wait_list = nullptr;
thread_local const void* tl_wait_owner = nullptr;

// The way out of a request: (response, file offset, file length or -1 = all, what to release on the I/O thread afterwards).
// The last argument is optional for callers.
// ponytail: the response travels by rvalue reference through every layer, so it is moved once (into the final owner), not once per layer.
struct send_fn : std::function<void(response&&, long long, long long, std::shared_ptr<void>)> {
    using base = std::function<void(response&&, long long, long long, std::shared_ptr<void>)>;
    using base::base;
    void operator()(response&& res, long long offset, long long length, std::shared_ptr<void> keep = {}) const {
        base::operator()(std::move(res), offset, length, std::move(keep));
    }
};

} // namespace

struct responder::impl {
    // One object per async request, one allocation of its own: the way out and the way to close are kept
    // as they are (no wrapper closure around them), and the 504 backstop entry is a plain pointer and number.
    send_fn send;                                   // hooks, then the engine
    detail::async_server::answer_fn answer;         // to close the connection when every copy was dropped unanswered
    std::shared_ptr<void> wait;                     // the list this request waits in (see server::impl::waiting)
    std::uint64_t wait_id = 0;
    // Forget this request (it no longer waits for an answer). Published last, with release, after `wait` and `wait_id` are set: track() runs after the
    // handler returned, so another thread may already be answering and reads these in settle_once(); seeing a non-null value means it sees the other two.
    std::atomic<void (*)(void* list, std::uint64_t id)> unwait{nullptr};
    std::atomic<bool> answered{false};
    std::atomic<bool> settled{false};
    void settle_once() {
        if (auto forget = unwait.load(std::memory_order_acquire); forget && !settled.exchange(true)) forget(wait.get(), wait_id);
    }
    ~impl() {
        if (!answered.load() && answer) { // every copy dropped without an answer: close, say nothing
            detail::wire_response none;
            none.status = 0;
            answer(std::move(none));
        }
        settle_once();
    }
};

void responder::send(response res) const {
    if (!impl_ || impl_->answered.exchange(true)) return; // only the first answer counts
    impl_->send(std::move(res), 0, -1, impl_); // impl_ rides along and is released on the I/O thread
    impl_->settle_once();
}

// ---- WebSocket upgrade: the pending decision (see ws_acceptor in httpp/websocket.hpp) ----
namespace {

bool has_token(const std::string& list, const char* token) {
    std::size_t pos = 0;
    while (pos <= list.size()) {
        std::size_t end = list.find(',', pos);
        if (end == std::string::npos) end = list.size();
        std::size_t a = pos, b = end;
        while (a < b && (list[a] == ' ' || list[a] == '\t')) ++a;
        while (b > a && (list[b - 1] == ' ' || list[b - 1] == '\t')) --b;
        if (ieq(list.substr(a, b - a), token)) return true;
        pos = end + 1;
    }
    return false;
}

} // namespace

namespace detail {

struct ws_upgrade {
    std::shared_ptr<responder::impl> state;  // the request that waits for its answer
    std::shared_ptr<server_request> req;
    std::shared_ptr<const ws_handler> handler;
    std::string key, offered;                // Sec-WebSocket-Key, and every Sec-WebSocket-Protocol the client sent

    void reject(response res) {
        if (state->answered.exchange(true)) return;  // only the first answer counts
        state->send(std::move(res), 0, -1, state);
        state->settle_once();
    }

    void accept(std::vector<std::pair<std::string, std::string>> extra, std::shared_ptr<void> user) {
        if (state->answered.load()) return;
        wire_response w;
        std::string problem;
        try {
            w.status = 101;
            w.headers.emplace_back("Upgrade", "websocket");
            w.headers.emplace_back("Connection", "Upgrade");
            w.headers.emplace_back("Sec-WebSocket-Accept", ws_accept_key(key));
            for (auto& [name, value] : extra) {
                if (ieq(name, "Upgrade") || ieq(name, "Connection") || ieq(name, "Sec-WebSocket-Accept") || ieq(name, "Content-Length") ||
                    ieq(name, "Transfer-Encoding") || ieq(name, "Date") || ieq(name, "Content-Type"))
                    continue;  // the handshake owns these
                if (!header_ok(name, value)) { problem = "a header with a line break"; break; }
                if (ieq(name, "Sec-WebSocket-Protocol") && !has_token(offered, value.c_str())) { problem = "a Sec-WebSocket-Protocol the client did not offer"; break; }
                w.headers.emplace_back(name, value);
            }
        } catch (...) {
            problem = "the handshake could not be built";
        }
        if (!problem.empty()) {
            response res;
            res.status = 500;
            res.body = "Internal Server Error";
            reject(std::move(res));
            throw std::invalid_argument("httpp::ws_acceptor::accept: " + problem);
        }
        auto h = handler;
        auto handle = std::make_shared<websocket>();
        ws_events ev;
        ev.on_open = [h, handle] { if (h->on_open) h->on_open(*handle); };
        ev.on_message = [h, handle](std::string_view m, bool binary) { if (h->on_message) h->on_message(*handle, m, binary); };
        ev.on_drain = [h, handle] { if (h->on_drain) h->on_drain(*handle); };
        ev.on_close = [h, handle](std::uint16_t code, std::string_view reason) { if (h->on_close) h->on_close(*handle, code, reason); };
        ws_limits limits;
        limits.max_message = h->max_message;
        limits.max_buffered = h->max_buffered;
        limits.ping_interval = h->ping_interval;
        auto session = ws_session::make(std::move(ev), limits, std::move(user));
        *handle = websocket(session->shared(), req);
        w.upgrade = std::move(session);
        if (state->answered.exchange(true)) return;  // lost the race with another answer
        state->answer(std::move(w));  // the 101 goes out as it is: the after hook does not run for it (see route_ws)
        state->settle_once();
    }
};

} // namespace detail

void ws_acceptor::accept(std::vector<std::pair<std::string, std::string>> headers, std::shared_ptr<void> user) const {
    if (upgrade_) upgrade_->accept(std::move(headers), std::move(user));
}
void ws_acceptor::reject(const response& res) const {
    if (upgrade_) upgrade_->reject(res);
}
void ws_acceptor::reject(int status, std::string_view body) const {
    response res;
    res.status = status;
    res.body = std::string(body.empty() ? (status == 403 ? "Forbidden" : "") : body);
    if (upgrade_) upgrade_->reject(std::move(res));
}

struct server::impl {
    std::shared_ptr<config> cfg = std::make_shared<config>();
    std::vector<route_entry> routes;

    // How a request finds its route without scanning them all (the answer is still "the first route, in
    // registration order, that fits"). Kept up to date by add_route().
    struct route_index {
        // The first route made only of literal segments, by method + path.
        std::unordered_map<std::string, std::size_t> literal;
        // The routes with a ":param", in registration order, by method + number of segments.
        std::unordered_map<std::string, std::vector<std::size_t>> parametric;
    } index;

    static bool is_param(const std::string& seg) { return seg.size() > 1 && seg[0] == ':'; }

    // Each segment is length-prefixed, so a decoded segment that contains a '/' cannot be mistaken for two.
    template <class Segs>
    static void literal_key(std::string& key, const std::string& method, const Segs& segs) {
        key.assign(method);
        char num[24];
        for (const auto& seg : segs) {
            const auto end = std::to_chars(num, num + sizeof num, seg.size()).ptr;
            key.push_back('/');
            key.append(num, end);
            key.push_back(':');
            key.append(seg);
        }
    }
    static void shape_key(std::string& key, const std::string& method, std::size_t count) {
        key.assign(method);
        key.push_back('#');
        char num[24];
        key.append(num, std::to_chars(num, num + sizeof num, count).ptr);
    }
    std::vector<mount_entry> mounts;
    std::size_t pool_base = std::max<std::size_t>(8, std::thread::hardware_concurrency());
    std::size_t pool_max = 0; // 0 = 4 x base
    std::chrono::microseconds pool_grow_after{500};
    bool tls_on = false; // set_tls() was called: the log says https
    std::atomic<bool> draining{false}; // shutdown() was called: an engine that finishes early must not stop the others (ADR-0038)
    std::atomic<int> async_timeout{60};

    // Requests that wait for the answer of a route_async / route_io handler, oldest first. They all have
    // the same timeout, so they expire in the order they came in and ONE timer can watch them all. (A
    // timer per request, which this used to be, left hundreds of thousands of live timers under load,
    // and that alone cost more than the whole rest of the request.)
    // ponytail: one list (one mutex, one timer) per I/O thread, so I/O threads never contend on it: a request is entered in the list of the thread
    // that dispatches it, and the engine of that thread sweeps it. Threads that are not I/O threads (pool workers) use the shared list `wait`.
    // Upgrade path: none needed.
    struct waiting : std::enable_shared_from_this<waiting> {
        detail::async_server* eng = nullptr; // the engine whose loop runs the sweep timer of this list
        struct entry {
            std::weak_ptr<responder::impl> state;
            std::shared_ptr<const server_request> req; // for the log line only; dropped when answered
            std::chrono::steady_clock::time_point deadline;
            int timeout_s;
        };
        std::mutex mu;
        std::map<std::uint64_t, entry> by_arrival; // key = arrival number
        std::uint64_t next = 0;
        bool armed = false; // a sweep is scheduled
    };
    std::shared_ptr<waiting> wait = std::make_shared<waiting>();            // for threads that are not I/O threads
    std::vector<std::shared_ptr<waiting>> waits;                            // one per engine, made when the server starts to listen

    // One engine per I/O thread. engines[0] is the one that runs on the thread that calls listen, owns the timers
    // (`engine`), and is the only one before set_io_threads() is called. Every setting that reaches an engine is
    // remembered in `applied` and replayed on engines made later.
    std::vector<std::unique_ptr<detail::async_server>> engines;
    detail::async_server* engine = nullptr; // engines[0]
    std::vector<std::function<void(detail::async_server&)>> applied;
    std::size_t max_conn_total = 0; // 0 = no limit; shared out over the engines
    std::atomic<detail::worker_pool*>& pool = cfg->pool; // alive only while listening; lives in the config so a late answer needs no `this`
    std::size_t io_route_count = 0;                  // routes added with route_io (set before listening)

    std::unique_ptr<detail::async_server> make_engine() {
        auto e = std::make_unique<detail::async_server>(
            detail::async_server::options{},
            [this](detail::http_request r, detail::async_server::answer_fn answer) {
                return on_request(std::move(r), std::move(answer));
            },
            [this](std::function<void()> job) {
                auto* p = pool.load();
                return p && p->enqueue(std::move(job));
            });
        for (auto& f : applied) f(*e);
        return e;
    }
    // Applies a setting to every engine, now and on the ones made later.
    template <class F>
    void configure(F f) {
        for (auto& e : engines) f(*e);
        applied.push_back(std::move(f));
    }
    void share_max_connections() {
        const std::size_t n = engines.size();
        const std::size_t each = max_conn_total == 0 ? 0 : (max_conn_total + n - 1) / n;
        for (auto& e : engines) e->set_max_connections(each);
    }
    // Binds every engine to the same port. Falls back to one engine when the others cannot share it.
    int bind_all(const std::string& host, int port) {
        if (engines.size() > 1) {
            for (auto& e : engines) e->set_reuse_port(true);
        }
        const int bound = engines[0]->bind(host, port);
        if (bound < 0) return bound;
        for (std::size_t i = 1; i < engines.size(); ++i) {
            if (engines[i]->bind(host, bound) < 0) {
                HTTPP_LOG(warning) << "the port cannot be shared between I/O threads here: using one I/O thread";
                engines.resize(1);
                break;
            }
        }
        share_max_connections();
        return bound;
    }

    impl() {
        engines.push_back(make_engine());
        engine = engines[0].get();
    }

    // ---- request ----
    // Takes the method, body and headers out of `hr` (it is the engine's copy, nobody needs it after).
    static bool make_request(detail::http_request& hr, server_request& req, std::string& raw_path) {
        req.method = std::move(hr.method);
        req.body = std::move(hr.body);
        req.secure = hr.secure;
        req.headers = std::move(hr.headers);
        std::string target = std::move(hr.target);  // put back below when the target is refused (the caller logs it)
        const std::size_t hash = target.find('#');
        if (hash != std::string::npos) target.resize(hash);
        if (std::string_view(target).substr(0, 7) == "http://" || std::string_view(target).substr(0, 8) == "https://") { // absolute form
            const std::size_t slash = target.find('/', target.find("//") + 2);
            target = slash == std::string::npos ? "/" : target.substr(slash);
        }
        if (target.empty() || (target[0] != '/' && std::string_view(target) != "*")) { hr.target = std::move(target); return false; }
        const std::size_t q = target.find('?');
        raw_path = target.substr(0, q);
        req.path = percent_decode(raw_path, false);
        if (req.path.find('\0') != std::string::npos) return false;
        if (q != std::string::npos) parse_query(target.substr(q + 1), req.query);
        for (const auto& [k, v] : req.headers) { // like cpp-httplib: form fields count as parameters
            if (ieq(k, "Content-Type") && v.compare(0, 33, "application/x-www-form-urlencoded") == 0)
                parse_query(req.body, req.query);
        }
        return true;
    }

    // ---- turning a handler's `response` into bytes for the engine ----
    static detail::wire_response to_wire(response&& res, long long offset, long long length) {
        detail::wire_response w;
        w.status = res.status > 0 ? res.status : 200;
        // ponytail: the handler's header vector is taken over as it is (no per-header moves, no new vector). Exactly one Content-Type is
        // sent, as the last header, with the value of the last one the handler set ("text/plain" when none): the usual case (one,
        // already last, or none) costs one scan. Upgrade path: none needed.
        w.headers = std::move(res.headers);
        std::size_t ct_count = 0, ct_last = 0;
        for (std::size_t i = 0; i < w.headers.size(); ++i) {
            if (!header_ok(w.headers[i].first, w.headers[i].second)) return failure(500, "Internal Server Error"); // never send a split header
            if (ieq(w.headers[i].first, "Content-Type")) { ++ct_count; ct_last = i; }
        }
        if (ct_count == 0) {
            w.headers.emplace_back("Content-Type", "text/plain");
        } else if (ct_count == 1) {
            if (ct_last + 1 != w.headers.size()) {
                auto moved = std::move(w.headers[ct_last]);
                w.headers.erase(w.headers.begin() + static_cast<std::ptrdiff_t>(ct_last));
                w.headers.push_back(std::move(moved));
            }
        } else {
            std::string value = std::move(w.headers[ct_last].second);
            w.headers.erase(std::remove_if(w.headers.begin(), w.headers.end(), [](const auto& h) { return ieq(h.first, "Content-Type"); }), w.headers.end());
            w.headers.emplace_back("Content-Type", std::move(value));
        }

        if (!res.file.empty()) {
            const long long size = file_size(res.file.str());
            if (size < 0) return failure(404, "Not Found");
            if (offset > size) offset = size;
            const long long len = length < 0 ? size - offset : std::min(length, size - offset);
            w.length = len;
            auto in = std::make_shared<std::ifstream>(res.file.native(), std::ios::binary);
            w.next = [in, pos = offset, end = offset + len](std::string& piece) mutable {
                if (pos >= end) return false;
                const std::size_t n = static_cast<std::size_t>(std::min<long long>(128 * 1024, end - pos));
                piece.resize(n);
                in->clear();
                in->seekg(static_cast<std::streamoff>(pos));
                in->read(&piece[0], static_cast<std::streamsize>(n));
                const std::size_t got = static_cast<std::size_t>(in->gcount());
                piece.resize(got);
                if (got == 0) throw std::runtime_error("file ended early"); // the connection is dropped
                pos += static_cast<long long>(got);
                return pos < end;
            };
        } else if (res.stream) {
            auto gen = std::make_shared<std::function<bool(std::size_t, std::string&)>>(std::move(res.stream));
            w.next = [gen, offset = std::size_t(0)](std::string& piece) mutable {
                const bool more = (*gen)(offset, piece);
                offset += piece.size();
                return more;
            };
        } else {
            w.body = std::move(res.body);
        }
        w.after_send = std::move(res.after_send);
        return w;
    }

    static detail::wire_response failure(int status, const char* text) {
        detail::wire_response w;
        w.status = status;
        w.headers.emplace_back("Content-Type", "text/plain");
        w.body = text;
        return w;
    }

    // The way out for every response: error handler, after hook, then the engine.
    send_fn make_sender(const std::shared_ptr<server_request>& req, detail::async_server::answer_fn answer) {
        if (!cfg->error && !cfg->after && !cfg->compression.enabled()) {
            // ponytail: the config is read-only once listening, so with no hook and no compression an answer needs nothing from it.
            // This sender holds no weak_ptr<config>: no atomic count on the one control block that every I/O thread would share, per request.
            // A late answer after the server is gone is as safe as before (the engine's answer_fn copes, ADR-0033 item 2).
            return [req, answer = std::move(answer)](response&& res, long long offset, long long length, std::shared_ptr<void> keep) {
                detail::wire_response w;
                try {
                    if (res.status <= 0) res.status = 200;
                    w = to_wire(std::move(res), offset, length);
                } catch (...) {
                    HTTPP_LOG(warning) << "response building failed (" << describe(std::current_exception()) << ") for " << req->method << ' ' << req->path << ": answering 500";
                    w = failure(500, "Internal Server Error");
                }
                w.keep = std::move(keep);
                answer(std::move(w));
            };
        }
        std::weak_ptr<config> weak = cfg;
        // ponytail: nothing here touches `this`: the server (and its pool) may be gone by the time a late answer arrives,
        // so everything the hook path needs lives in the shared config (ADR-0035).
        return [weak, req, answer = std::move(answer)](response&& res, long long offset, long long length, std::shared_ptr<void> keep) {
            auto c = weak.lock();
            if (!c) return; // the server is gone
            answer_with_hooks(c, req, answer, std::move(res), offset, length, std::move(keep), tl_on_io_thread);
        };
    }

    static void answer_with_hooks(const std::shared_ptr<config>& c, const std::shared_ptr<server_request>& req, const detail::async_server::answer_fn& answer,
                                  response&& res, long long offset, long long length, std::shared_ptr<void> keep, bool on_io_thread) {
        if ((c->error || c->after || c->compression.worth_a_thread(res)) && on_io_thread) {
            // A route_io handler answered on the spot. The hooks may block (Python code waits for
            // the GIL) and compressing a big body takes a while; the I/O thread must never wait: a worker finishes the job.
            auto* p = c->pool.load();
            auto job = [c, req, answer, res = std::move(res), offset, length, keep = std::move(keep)]() mutable {
                answer_with_hooks(c, req, answer, std::move(res), offset, length, std::move(keep), false);
            };
            if (p && p->enqueue(job)) return; // enqueue copied the job, so `job` still owns the response below
            job(); // no pool (shutting down): carry on here rather than lose the answer
            return;
        }
        detail::wire_response w;
        try {
            if (res.status <= 0) res.status = 200;
            if (c->error && res.status >= 400 && res.body.empty() && res.file.empty() && !res.stream) {
                // Only errors the server made itself (no body yet) are ours to dress up.
                response dressed;
                dressed.status = res.status;
                try {
                    c->error(*req, dressed);
                    res.status = dressed.status;
                    res.headers = std::move(dressed.headers);
                    res.body = std::move(dressed.body);
                    res.file = std::move(dressed.file);
                    res.stream = std::move(dressed.stream);
                } catch (...) { // a broken error handler keeps the plain error
                    HTTPP_LOG(warning) << "error handler threw (" << describe(std::current_exception()) << "); keeping the plain " << res.status << " for " << req->method << ' ' << req->path;
                }
            }
            if (c->after) {
                response out; // status and headers only: the body may be huge or streamed
                out.status = res.status;
                out.headers = res.headers;
                c->after(*req, out);
                res.status = out.status;
                res.headers = std::move(out.headers);
            }
            c->compression.apply(*req, res); // after the hooks, so they see the plain headers; a no-op when off
            w = to_wire(std::move(res), offset, length);
        } catch (...) {
            HTTPP_LOG(warning) << "after hook or response building failed (" << describe(std::current_exception()) << ") for " << req->method << ' ' << req->path << ": answering 500";
            w = failure(500, "Internal Server Error");
        }
        w.keep = std::move(keep);
        answer(std::move(w));
    }

    void on_exception(const std::shared_ptr<server_request>& req, const send_fn& send, std::exception_ptr ep) {
        response out;
        out.status = 500;
        out.body = "Internal Server Error";
        if (cfg->exception) {
            HTTPP_LOG(debug) << "handler for " << req->method << ' ' << req->path << " threw (" << describe(ep) << "); exception handler runs";
            try {
                cfg->exception(*req, out, ep);
            } catch (...) {
                HTTPP_LOG(warning) << "exception handler threw (" << describe(std::current_exception()) << "): answering 500";
                out = response{};
                out.status = 500;
                out.body = "Internal Server Error";
            }
        } else {
            HTTPP_LOG(warning) << "handler for " << req->method << ' ' << req->path << " threw: " << describe(ep);
        }
        send(std::move(out), 0, -1);
    }

    // ---- routing ----
    // First registered route that fits wins. Segments are compared decoded, one by one, so an
    // encoded slash stays inside its segment.
    const route_entry* match(const std::string& method, const std::string& raw_path,
                             std::vector<std::pair<std::string, std::string>>& params) const {
        if (raw_path.empty() || raw_path[0] != '/') return nullptr;
        std::vector<std::string> parts = split_path(raw_path);
        for (auto& part : parts)
            if (part.find('%') != std::string::npos) part = percent_decode(part, false);

        thread_local std::string key; // reused, so a lookup does not allocate once it is warm
        constexpr std::size_t none = static_cast<std::size_t>(-1);
        std::size_t best = none; // the literal route, if any: a parameter route registered earlier still beats it
        if (!index.literal.empty()) {
            literal_key(key, method, parts);
            if (auto it = index.literal.find(key); it != index.literal.end()) best = it->second;
        }
        shape_key(key, method, parts.size());
        if (auto it = index.parametric.find(key); it != index.parametric.end()) {
            for (const std::size_t i : it->second) { // ascending: the first fit wins
                if (i > best) break;
                const route_entry& r = routes[i];
                std::vector<std::pair<std::string, std::string>> found;
                bool ok = true;
                for (std::size_t k = 0; k < parts.size() && ok; ++k) {
                    const std::string& seg = r.segs[k];
                    if (is_param(seg)) {
                        if (parts[k].empty()) ok = false;
                        else found.emplace_back(seg.substr(1), parts[k]);
                    } else {
                        ok = seg == parts[k];
                    }
                }
                if (ok) {
                    params = std::move(found);
                    return &r;
                }
            }
        }
        if (best == none) return nullptr;
        params.clear();
        return &routes[best];
    }

    // The methods that have a route for this path (for a 405 answer). Only runs when nothing matched,
    // so a normal request never pays for it. A GET route also answers HEAD.
    std::string allowed_methods(const std::string& raw_path) const {
        std::set<std::string> methods;
        for (const route_entry& r : routes) methods.insert(r.method);
        std::set<std::string> allowed;
        std::vector<std::pair<std::string, std::string>> scratch;
        for (const std::string& m : methods) {
            if (match(m, raw_path, scratch)) {
                allowed.insert(m);
                if (m == "GET") allowed.insert("HEAD");
            }
        }
        std::string out;
        for (const std::string& m : allowed) out += (out.empty() ? "" : ", ") + m;
        return out;
    }

    // ---- static files ----
    enum class range_result { none, ok, unsatisfiable };
    static range_result parse_range(const std::string& header, long long size, long long& start, long long& end) {
        if (header.compare(0, 6, "bytes=") != 0 || header.find(',') != std::string::npos) return range_result::none;
        const std::string spec = header.substr(6);
        const std::size_t dash = spec.find('-');
        if (dash == std::string::npos) return range_result::none;
        const std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
        auto digits = [](const std::string& t) {
            return !t.empty() && t.size() < 19 && std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        };
        if ((!a.empty() && !digits(a)) || (!b.empty() && !digits(b)) || (a.empty() && b.empty())) return range_result::none;
        if (a.empty()) { // the last N bytes
            const long long n = std::stoll(b);
            if (n == 0 || size == 0) return range_result::unsatisfiable;
            start = std::max<long long>(0, size - n);
            end = size - 1;
            return range_result::ok;
        }
        start = std::stoll(a);
        if (start >= size) return range_result::unsatisfiable;
        end = b.empty() ? size - 1 : std::min<long long>(std::stoll(b), size - 1);
        return end < start ? range_result::none : range_result::ok;
    }

    static bool under_prefix(const std::string& p, const std::string& prefix) {
        return p.compare(0, prefix.size(), prefix) == 0 && (p.size() == prefix.size() || p[prefix.size()] == '/'); // "/filesX" is not under "/files"
    }
    // A GET/HEAD for this path might be answered from a mounted directory (which means reading the disk).
    bool may_be_static(const server_request& req) const {
        if (std::string_view(req.method) != "GET" && std::string_view(req.method) != "HEAD") return false;
        for (const auto& m : mounts) if (under_prefix(req.path, m.prefix)) return true;
        return false;
    }

    // True when `file` (symlinks resolved) is still inside `dir` (resolved too). A file that does not
    // exist counts as inside: the caller's file_size() answers that case.
    static bool inside_dir(const std::string& file, const std::string& dir) {
        const httpp::path real = httpp::path(file).resolve();
        if (real.empty()) return true;
        const httpp::path root = httpp::path(dir.empty() ? "." : dir).resolve();
        return !root.empty() && (root.is_root() || real.is_relative_to(root));
    }

    // A file shown inline must not be able to run code in the page's origin: only types that cannot are shown, the rest is a download.
    static void add_download_headers(const server_request& req, const std::string& full, response& res) {
        const std::string type = mime_for(full);
        const bool plain = type == "text/plain" || type == "application/json" || type == "text/csv" || type == "text/markdown" || type == "application/pdf" ||
                           (type.compare(0, 6, "image/") == 0 && type != "image/svg+xml") || type.compare(0, 6, "audio/") == 0 || type.compare(0, 6, "video/") == 0;
        res.headers.emplace_back("X-Content-Type-Options", "nosniff");
        if (plain && req.query_param("download") != "1") return;
        const std::size_t slash = full.find_last_of("/\\");
        const std::string name = slash == std::string::npos ? full : full.substr(slash + 1);
        std::string ascii, encoded;
        static const char hex[] = "0123456789ABCDEF";
        for (unsigned char c : name) {
            const bool unreserved = std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '~';
            if (unreserved) encoded += static_cast<char>(c); else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
            ascii += (c >= 0x20 && c < 0x7f && c != '"' && c != '\\' && c != '%') ? static_cast<char>(c) : '_';
        }
        res.headers.emplace_back("Content-Disposition", "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + encoded);
    }

    bool serve_static(const server_request& req, const send_fn& send) {
        for (const auto& m : mounts) {
            const std::string& p = req.path;
            if (!under_prefix(p, m.prefix)) continue;
            const std::string rel = p.substr(m.prefix.size()); // "" or "/a/b"
            if (rel.find('\\') != std::string::npos || rel.find('\0') != std::string::npos) continue; // a NUL would end the file name early
            bool safe = true;
            for (const auto& seg : split_path(rel.empty() ? "/" : rel)) {
                if (seg == "..") safe = false;
                if (!m.options.hidden && !seg.empty() && seg[0] == '.') safe = false; // also "." and ".hidden"
            }
            if (!safe) continue; // never leave the mounted directory (and never show what the mount keeps hidden)
            const bool names_a_folder = rel.empty() || rel.back() == '/';
            if (names_a_folder && !m.options.index) continue;
            std::string full = m.dir;
            if (!full.empty() && full.back() != '/' && full.back() != '\\') full += '/';
            full += rel.empty() ? "" : rel.substr(1);
            if (names_a_folder) full += "index.html";
            if (!inside_dir(full, m.dir)) continue; // a symlink must not lead out of the mounted directory
            const long long size = file_size(full);
            if (size < 0) continue;
            return answer_file(req, send, full, size, m.options);
        }
        return false;
    }

    // The answer for one existing file: validators, conditional requests (ADR-0045), then the whole file or a range.
    bool answer_file(const server_request& req, const send_fn& send, const std::string& full, long long size, const server::directory_options& options) {
        {
            const httpp::path::status stamp = httpp::path(full).stat();
            const bool stamped = stamp.type == httpp::path::kind::file;
            const std::string etag = stamped ? make_etag(size, stamp) : std::string();
            const std::string modified = stamped ? format_http_date(stamp.mtime) : std::string();
            bool honour_range = true;
            if (stamped) {
                const precondition p = evaluate_preconditions(req, etag, stamp.mtime, honour_range);
                if (p != precondition::proceed) {
                    response out;
                    out.status = p == precondition::failed ? 412 : 304;
                    if (p == precondition::not_modified) { // the validators go with a 304 (RFC 9110 section 15.4.5)
                        out.headers.emplace_back("ETag", etag);
                        out.headers.emplace_back("Last-Modified", modified);
                    }
                    send(std::move(out), 0, -1);
                    return true;
                }
            }

            response res;
            res.status = 200;
            res.file = full;
            res.headers.emplace_back("Content-Type", mime_for(full));
            res.headers.emplace_back("Accept-Ranges", "bytes");
            if (stamped) {
                res.headers.emplace_back("ETag", etag);
                res.headers.emplace_back("Last-Modified", modified);
            }
            if (options.download_active) add_download_headers(req, full, res);
            long long start = 0, end = size - 1;
            switch (honour_range ? parse_range(req.header("Range"), size, start, end) : range_result::none) {
                case range_result::unsatisfiable: {
                    response bad;
                    bad.status = 416;
                    bad.headers.emplace_back("Content-Range", "bytes */" + std::to_string(size));
                    send(std::move(bad), 0, -1);
                    return true;
                }
                case range_result::ok:
                    res.status = 206;
                    res.headers.emplace_back("Content-Range", "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(size));
                    send(std::move(res), start, end - start + 1);
                    return true;
                case range_result::none: break;
            }
            send(std::move(res), 0, -1);
            return true;
        }
    }

    // ---- the 504 backstop ----

    void track(const std::shared_ptr<responder::impl>& state, const std::shared_ptr<server_request>& req, int timeout_s) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
        std::uint64_t id;
        bool arm = false;
        std::shared_ptr<waiting> w = (tl_wait_owner == this && tl_wait_list) ? static_cast<waiting*>(tl_wait_list)->shared_from_this() : wait;
        {
            std::lock_guard<std::mutex> lock(w->mu);
            id = w->next++;
            w->by_arrival.emplace_hint(w->by_arrival.end(), id, waiting::entry{state, req, deadline, timeout_s});
            if (!w->armed) w->armed = arm = true;
        }
        // erasing an id that a sweep already took out is harmless
        state->wait = w;
        state->wait_id = id;
        state->unwait.store([](void* list, std::uint64_t at) {
            auto* l = static_cast<waiting*>(list);
            std::lock_guard<std::mutex> lock(l->mu);
            l->by_arrival.erase(at);
        }, std::memory_order_release);
        if (arm) arm_sweep(*w, deadline);
    }

    void arm_sweep(waiting& w, std::chrono::steady_clock::time_point at) {
        auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(at - std::chrono::steady_clock::now());
        if (delay < std::chrono::milliseconds(10)) delay = std::chrono::milliseconds(10);
        w.eng->after(delay, [this, list = &w] { sweep(*list); });
    }

    // On the loop thread. Takes the expired requests out of the queue and answers them 504 (on a worker:
    // the hooks and the write may block), then schedules the next sweep for the oldest one still waiting.
    void sweep(waiting& list) {
        std::vector<std::pair<std::shared_ptr<responder::impl>, waiting::entry>> late;
        bool more = false;
        std::chrono::steady_clock::time_point next;
        {
            std::lock_guard<std::mutex> lock(list.mu);
            const auto now = std::chrono::steady_clock::now();
            auto it = list.by_arrival.begin();
            for (; it != list.by_arrival.end() && it->second.deadline <= now; ++it) {
                if (auto st = it->second.state.lock()) late.emplace_back(std::move(st), std::move(it->second));
            }
            list.by_arrival.erase(list.by_arrival.begin(), it);
            more = !list.by_arrival.empty();
            if (more) next = list.by_arrival.begin()->second.deadline;
            list.armed = more;
        }
        if (more) arm_sweep(list, next);
        for (auto& item : late) {
            auto answer_504 = [st = std::move(item.first), req = std::move(item.second.req), timeout = item.second.timeout_s] {
                if (!st->answered.load()) { HTTPP_LOG(warning) << "async handler for " << req->method << ' ' << req->path << " did not answer within " << timeout << " s: answering 504"; }
                responder late_responder;
                late_responder.impl_ = st;
                response err;
                err.status = 504;
                err.body = "Gateway Timeout";
                late_responder.send(std::move(err)); // no-op if it was answered after all
            };
            if (auto* p = pool.load(); !p || !p->enqueue(answer_504)) answer_504();
        }
    }

    // ---- running a request ----

    // Runs on the I/O thread for every complete request, and must not block. Requests that need a worker
    // (everything but a route_io route) are handed to the pool, already parsed and matched.
    bool on_request(detail::http_request hr, detail::async_server::answer_fn answer) {
        auto* p = pool.load();
        if (!p) return false;
        // A before hook may block and may answer first, and without route_io routes there is nothing to
        // gain: the whole request is handled on a worker, which parses it there.
        if (io_route_count == 0 || cfg->before) {
            return p->enqueue([this, hr = std::move(hr), answer = std::move(answer)]() mutable {
                handle(std::move(hr), std::move(answer));
            });
        }
        prepared pr;
        pr.req = std::make_shared<server_request>();
        if (!make_request(hr, *pr.req, pr.raw_path)) {
            HTTPP_LOG(debug) << "bad request target: " << hr.target.substr(0, 100);
            answer(failure(400, "Bad Request"));
            return true;
        }
        if (!may_be_static(*pr.req)) { // else the worker looks at the disk first, then matches
            const std::string method = std::string_view(pr.req->method) == "HEAD" ? "GET" : pr.req->method; // HEAD runs the GET route
            pr.route = match(method, pr.raw_path, pr.req->path_params);
            pr.matched = true;
            if (pr.route && pr.route->on_io_thread()) { // right here: such a handler must not block
                run(std::move(pr), std::move(answer));
                return true;
            }
        }
        return p->enqueue([this, pr = std::move(pr), answer = std::move(answer)]() mutable {
            run(std::move(pr), std::move(answer));
        });
    }

    // Runs on a pool thread: nothing is parsed yet.
    void handle(detail::http_request hr, detail::async_server::answer_fn answer) {
        prepared pr;
        pr.req = std::make_shared<server_request>();
        if (!make_request(hr, *pr.req, pr.raw_path)) {
            HTTPP_LOG(debug) << "bad request target: " << hr.target.substr(0, 100);
            return answer(failure(400, "Bad Request"));
        }
        run(std::move(pr), std::move(answer));
    }

    // Before hook, mounted directories, route, handler. On a pool thread, except for a route_io route
    // that on_request() ran straight on the I/O thread: that has no before hook and no file to read.
    // (If such a handler answers on the spot, make_sender moves the after/error hooks to a worker.)
    void run(prepared pr, detail::async_server::answer_fn answer) {
        const std::shared_ptr<server_request>& req = pr.req;
        send_fn send = make_sender(req, answer);

        bool handled = false;
        response early;
        try {
            if (cfg->before) {
                early.status = 0; // sent as 200 unless the hook sets it
                handled = cfg->before(*req, early);
            }
        } catch (...) {
            return on_exception(req, send, std::current_exception());
        }
        if (handled) return send(std::move(early), 0, -1);

        if ((std::string_view(req->method) == "GET" || std::string_view(req->method) == "HEAD") && serve_static(*req, send)) return;

        const route_entry* r = pr.route;
        if (!pr.matched) {
            const std::string method = std::string_view(req->method) == "HEAD" ? "GET" : req->method; // HEAD runs the GET route; the engine drops the body
            r = match(method, pr.raw_path, req->path_params);
        }
        if (!r) {
            response nf;
            nf.status = 404;
            // ponytail: one scan per miss, no index; a miss is not the hot path
            if (const std::string allow = allowed_methods(pr.raw_path); !allow.empty()) {
                nf.status = 405;
                nf.headers.emplace_back("Allow", allow);
            }
            return send(std::move(nf), 0, -1);
        }

        if (r->ws) return run_ws(req, send, std::move(answer), r->ws);

        if (r->h || r->sh) {
            response res;
            res.status = 200;
            try {
                if (r->sh) r->sh(req, res);
                else r->h(*req, res);
            } catch (...) {
                return on_exception(req, send, std::current_exception());
            }
            return send(std::move(res), 0, -1);
        }

        // Asynchronous route: the handler may return at once; whoever holds the responder answers later.
        auto state = std::make_shared<responder::impl>();
        state->send = std::move(send);
        state->answer = std::move(answer);
        responder rsp;
        rsp.impl_ = state;

        const int timeout = async_timeout.load();

        try {
            if (r->ih) r->ih(req, rsp);
            else r->ah(*req, rsp);
        } catch (...) {
            response err;
            err.status = 500;
            err.body = "Internal Server Error";
            const std::exception_ptr ep = std::current_exception();
            if (cfg->exception) {
                try {
                    cfg->exception(*req, err, ep);
                } catch (...) {
                    HTTPP_LOG(warning) << "exception handler threw (" << describe(std::current_exception()) << "): answering 500";
                    err = response{};
                    err.status = 500;
                    err.body = "Internal Server Error";
                }
            } else {
                HTTPP_LOG(warning) << "async handler for " << req->method << ' ' << req->path << " threw: " << describe(ep);
            }
            rsp.send(std::move(err)); // no-op if the handler had already answered
        }

        // If nobody answers in time, answer 504 ourselves (see `waiting`). ponytail: only a request that is still unanswered when its
        // handler returns is entered in the list: an answer made on the spot (the usual route_io case) costs no lock, no list node and
        // no count. The deadline starts when the handler returns instead of at arrival, which differs by the handler's own run time
        // (it must not block). An answer that races with the entry is settled right after, so no stale entry stays behind.
        if (timeout > 0 && !state->answered.load()) {
            track(state, req, timeout);
            if (state->answered.load()) state->settle_once();
        }
    }


    // ---- WebSocket handshake (RFC 6455 section 4.2.1), then the connection belongs to a ws_session ----
    // The protocol checks happen here, on the I/O thread. The decision to accept is the handler's (ws_handler::on_handshake), possibly later
    // and from another thread: the request waits like a route_async request (same 504 timeout, same fate when every acceptor copy is dropped).
    void run_ws(const std::shared_ptr<server_request>& req, const send_fn& send, detail::async_server::answer_fn answer,
                const std::shared_ptr<const ws_handler>& h) {
        auto refuse = [&](int status, const char* text, std::vector<std::pair<std::string, std::string>> headers = {}) {
            response res;
            res.status = status;
            res.body = text;
            res.headers = std::move(headers);
            send(std::move(res), 0, -1);
        };
        bool upgrade = false, connection = false;
        std::string key, version, offered;
        for (const auto& [name, value] : req->headers) {
            if (ieq(name, "Upgrade") && has_token(value, "websocket")) upgrade = true;
            else if (ieq(name, "Connection") && has_token(value, "upgrade")) connection = true;
            else if (ieq(name, "Sec-WebSocket-Key")) key = value;
            else if (ieq(name, "Sec-WebSocket-Version")) version = value;
            else if (ieq(name, "Sec-WebSocket-Protocol")) offered += (offered.empty() ? "" : ",") + value;
        }
        if (req->method != "GET" || !upgrade || !connection || !detail::ws_key_is_valid(key)) return refuse(400, "Bad Request");
        if (version != "13") return refuse(426, "Upgrade Required", {{"Sec-WebSocket-Version", "13"}});

        auto state = std::make_shared<responder::impl>();
        state->send = send;
        state->answer = std::move(answer);
        auto up = std::make_shared<detail::ws_upgrade>();
        up->state = state;
        up->req = req;
        up->handler = h;
        up->key = std::move(key);
        up->offered = std::move(offered);
        if (!h->on_handshake) return up->accept({}, {});

        const int timeout = async_timeout.load();
        if (timeout > 0) track(state, req, timeout);
        try {
            h->on_handshake(req, ws_acceptor(up));
        } catch (...) {
            const std::exception_ptr ep = std::current_exception();
            response err;
            err.status = 500;
            err.body = "Internal Server Error";
            if (cfg->exception) {
                try {
                    cfg->exception(*req, err, ep);
                } catch (...) {
                    err = response{};
                    err.status = 500;
                    err.body = "Internal Server Error";
                }
            } else {
                HTTPP_LOG(warning) << "websocket on_handshake for " << req->path << " threw: " << describe(ep);
            }
            up->reject(std::move(err));  // no-op if it had answered already
        }
    }

    void add_route(const std::string& method, const std::string& path, route_entry r) {
        std::string m = method;
        std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return std::toupper(c); });
        if (m != "GET" && m != "POST" && m != "PUT" && m != "PATCH" && m != "DELETE" && m != "OPTIONS")
            throw std::invalid_argument("httpp::server::route: unsupported method '" + method + "'");
        r.method = std::move(m);
        r.segs = split_path(!path.empty() && path[0] == '/' ? path : "/" + path);
        if (r.ih || r.ws) ++io_route_count;
        const std::size_t at = routes.size();
        std::string key;
        if (std::any_of(r.segs.begin(), r.segs.end(), is_param)) {
            shape_key(key, r.method, r.segs.size());
            index.parametric[key].push_back(at);
        } else {
            literal_key(key, r.method, r.segs);
            index.literal.emplace(key, at); // emplace keeps the first one if the same route is added twice
        }
        routes.push_back(std::move(r));
    }
};

server::server() : impl_(std::make_unique<impl>()) {}
server::~server() = default;
server::server(server&&) noexcept = default;
server& server::operator=(server&&) noexcept = default;

void server::route(const std::string& method, const std::string& path, handler h) {
    route_entry r;
    r.h = std::move(h);
    impl_->add_route(method, path, std::move(r));
}
void server::route_shared(const std::string& method, const std::string& path, shared_handler h) {
    route_entry r;
    r.sh = std::move(h);
    impl_->add_route(method, path, std::move(r));
}
void server::route_async(const std::string& method, const std::string& path, async_handler h) {
    route_entry r;
    r.ah = std::move(h);
    impl_->add_route(method, path, std::move(r));
}
void server::route_io(const std::string& method, const std::string& path, io_handler h) {
    route_entry r;
    r.ih = std::move(h);
    impl_->add_route(method, path, std::move(r));
}

void server::route_ws(const std::string& path, ws_handler h) {
    route_entry r;
    r.ws = std::make_shared<const ws_handler>(std::move(h));
    impl_->add_route("GET", path, std::move(r));
}

void server::get(const std::string& path, handler h) { route("GET", path, std::move(h)); }
void server::post(const std::string& path, handler h) { route("POST", path, std::move(h)); }
void server::put(const std::string& path, handler h) { route("PUT", path, std::move(h)); }
void server::patch(const std::string& path, handler h) { route("PATCH", path, std::move(h)); }
void server::del(const std::string& path, handler h) { route("DELETE", path, std::move(h)); }

void server::get_async(const std::string& path, async_handler h) { route_async("GET", path, std::move(h)); }
void server::post_async(const std::string& path, async_handler h) { route_async("POST", path, std::move(h)); }
void server::put_async(const std::string& path, async_handler h) { route_async("PUT", path, std::move(h)); }
void server::patch_async(const std::string& path, async_handler h) { route_async("PATCH", path, std::move(h)); }
void server::del_async(const std::string& path, async_handler h) { route_async("DELETE", path, std::move(h)); }

// ponytail: composing once per change keeps the per-request path at one std::function call, and no call at all when
// nothing is registered (the engine's fast path checks for an empty hook). Upgrade path: none needed.
static void rebuild_hooks(config& c) {
    if (c.mw_before.empty()) c.before = c.app_before;
    else c.before = [mws = c.mw_before, app = c.app_before](const server_request& req, response& res) {
        for (const auto& h : mws) if (h(req, res)) return true;
        return app ? app(req, res) : false;
    };
    if (c.mw_after.empty()) c.after = c.app_after;
    else c.after = [mws = c.mw_after, app = c.app_after](const server_request& req, response& res) {
        if (app) app(req, res);
        for (auto it = mws.rbegin(); it != mws.rend(); ++it) (*it)(req, res);
    };
}

server& server::before(before_hook hook) { impl_->cfg->app_before = std::move(hook); rebuild_hooks(*impl_->cfg); return *this; }
server& server::after(after_hook hook) { impl_->cfg->app_after = std::move(hook); rebuild_hooks(*impl_->cfg); return *this; }
server& server::use(before_hook before, after_hook after) {
    config& c = *impl_->cfg;
    if (before) c.mw_before.push_back(std::move(before));
    if (after) c.mw_after.push_back(std::move(after));
    rebuild_hooks(c);
    return *this;
}
server& server::set_error_handler(error_hook hook) { impl_->cfg->error = std::move(hook); return *this; }
server& server::set_exception_handler(exception_hook hook) { impl_->cfg->exception = std::move(hook); return *this; }

server& server::set_thread_pool(std::size_t base, std::size_t max, std::chrono::microseconds grow_after) {
    if (base == 0) base = 1;
    if (max < base) max = base * 4;
    impl_->pool_base = base;
    impl_->pool_max = max;
    impl_->pool_grow_after = grow_after.count() > 0 ? grow_after : std::chrono::microseconds(1);
    return *this;
}
server& server::set_read_timeout(int seconds) { impl_->configure([seconds](detail::async_server& e) { e.set_read_timeout(seconds); }); return *this; }
server& server::set_write_timeout(int seconds) { impl_->configure([seconds](detail::async_server& e) { e.set_write_timeout(seconds); }); return *this; }
server& server::set_keep_alive_timeout(int seconds) { impl_->configure([seconds](detail::async_server& e) { e.set_keep_alive_timeout(seconds); }); return *this; }
server& server::set_keep_alive_max(std::size_t requests) { impl_->configure([requests](detail::async_server& e) { e.set_keep_alive_max(requests); }); return *this; }
server& server::set_max_body(std::size_t bytes) { impl_->configure([bytes](detail::async_server& e) { e.set_max_body(bytes); }); return *this; }
server& server::set_tls(const path& cert_file, const path& key_file, const std::string& key_password) {
    std::string error;
    auto ctx = detail::make_tls_server_context(cert_file.str(), key_file.str(), key_password, error);
    if (!ctx) throw std::runtime_error("set_tls: " + error);
    impl_->configure([ctx](detail::async_server& e) { e.set_tls(ctx); });
    impl_->tls_on = true;
    return *this;
}
server& server::set_header_timeout(int seconds) { impl_->configure([seconds](detail::async_server& e) { e.set_header_timeout(seconds); }); return *this; }
server& server::set_body_timeout(int seconds) { impl_->configure([seconds](detail::async_server& e) { e.set_body_timeout(seconds); }); return *this; }
server& server::set_max_connections(std::size_t n) { impl_->max_conn_total = n; impl_->share_max_connections(); return *this; }
server& server::set_reuse_port(bool on) { impl_->configure([on](detail::async_server& e) { e.set_reuse_port(on); }); return *this; }
server& server::set_io_threads(std::size_t n) {
#ifdef _WIN32
    n = 1; // no SO_REUSEPORT to share the port with
#endif
    if (n == 0) n = 1;
    if (n > 256) n = 256;
    while (impl_->engines.size() < n) impl_->engines.push_back(impl_->make_engine());
    impl_->engines.resize(n);
    impl_->engine = impl_->engines[0].get();
    impl_->share_max_connections();
    return *this;
}
server& server::set_compression(std::size_t min_size, int level) {
    impl_->cfg->compression.configure(min_size, level);
    return *this;
}
server& server::set_async_timeout(int seconds) {
    impl_->async_timeout = seconds < 0 ? 0 : seconds;
    return *this;
}

void server::serve_directory(const std::string& mount_path, const std::string& local_dir) {
    serve_directory(mount_path, local_dir, directory_options{});
}

void server::serve_directory(const std::string& mount_path, const std::string& local_dir, const directory_options& options) {
    std::string prefix = mount_path;
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back(); // "/" -> "" (the root)
    if (!prefix.empty() && prefix[0] != '/') prefix = "/" + prefix;
    impl_->mounts.push_back({std::move(prefix), local_dir, options});
}

int server::bind_to_any_port(const std::string& host) {
    const int port = impl_->bind_all(host, 0);
    if (port >= 0) { HTTPP_LOG(info) << "listening on " << (impl_->tls_on ? "https" : "http") << "://" << host << ':' << port; }
    return port;
}

bool server::listen_after_bind() {
    impl_->draining = false;
    const std::size_t base = impl_->pool_base;
    const std::size_t max = impl_->pool_max ? impl_->pool_max : base * 4;
    detail::worker_pool pool(base, max, impl_->pool_grow_after);
    impl_->pool = &pool;
    // Every I/O thread gets the same number of connections (the kernel's SO_REUSEPORT hash does not give that).
    {
        std::vector<detail::async_server*> all;
        for (auto& e : impl_->engines) all.push_back(e.get());
        for (auto& e : impl_->engines) e->balance_with(all);
    }
    // One waiting list per engine (see server::impl::waiting). A previous run's timers died with their loops, so every list starts empty.
    impl_->waits.clear();
    for (auto& e : impl_->engines) {
        auto l = std::make_shared<server::impl::waiting>();
        l->eng = e.get();
        impl_->waits.push_back(std::move(l));
    }
    {
        std::lock_guard<std::mutex> lock(impl_->wait->mu);
        impl_->wait->by_arrival.clear();
        impl_->wait->armed = false;
        impl_->wait->eng = impl_->engines[0].get();
    }
    // The other I/O threads; this thread runs engines[0] (and owns the timers).
    std::vector<std::thread> more;
    std::atomic<bool> others_ok{true};
    for (std::size_t i = 1; i < impl_->engines.size(); ++i) {
        detail::async_server* e = impl_->engines[i].get();
        more.emplace_back([e, &others_ok, owner = impl_.get(), list = impl_->waits[i].get()] {
            tl_on_io_thread = true;
            tl_wait_owner = owner;
            tl_wait_list = list;
            if (!e->run()) others_ok = false;
            tl_wait_list = nullptr;
            tl_wait_owner = nullptr;
            tl_on_io_thread = false;
        });
    }
    tl_on_io_thread = true; // the engine's loop runs on this very thread
    tl_wait_owner = impl_.get();
    tl_wait_list = impl_->waits[0].get();
    const bool ok = impl_->engines[0]->run(); // returns after stop()
    tl_wait_list = nullptr;
    tl_wait_owner = nullptr;
    tl_on_io_thread = false;
    // If this one ended on its own (a failure), the others must end too. After shutdown() each engine ends when its own
    // connections are gone or its grace period is over, so one that finishes first must not cut the others.
    if (!ok || !impl_->draining.load()) for (auto& e : impl_->engines) e->stop();
    for (auto& t : more) t.join();
    impl_->pool = nullptr;
    pool.shutdown(); // joins the workers
    return ok && others_ok.load();
}

bool server::listen(const std::string& host, int port) {
    const int bound = impl_->bind_all(host, port);
    if (bound < 0) {
        HTTPP_LOG(error) << "cannot listen on " << host << ':' << port;
        return false;
    }
    HTTPP_LOG(info) << "listening on " << (impl_->tls_on ? "https" : "http") << "://" << host << ':' << bound;  // after the bind, so a taken port never says it
    return listen_after_bind();
}

void server::stop() { for (auto& e : impl_->engines) e->stop(); }
void server::shutdown(std::chrono::milliseconds grace) {
    impl_->draining = true;
    for (auto& e : impl_->engines) e->drain(grace);
}

const char* mime_type_for(const std::string& path) { return mime_for(path); }

} // namespace httpp
