#include "httpp/server.hpp"

// httpp::server on top of the async engine (internal/async_server.hpp: Asio + llhttp). Routing,
// static files, hooks and response building live here; the engine only moves bytes. No
// cpp-httplib: this translation unit does not include it.

#include "internal/async_server.hpp"
#include "internal/log.hpp"
#include "internal/response_compression.hpp"
#include "internal/urlencoded.hpp"
#include "internal/worker_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#ifdef _WIN32
#include <filesystem>
#else
#include <climits>
#include <cstdlib>
#endif
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

bool ieq(const std::string& a, const char* b) {
    const std::size_t n = std::strlen(b);
    return a.size() == n && std::equal(a.begin(), a.end(), b, [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
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

bool header_ok(const std::string& name, const std::string& value) {
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
        {"mp4", "video/mp4"}, {"webm", "video/webm"},
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

// Size of a readable regular file, or -1. A directory opens fine but cannot be read: peek turns that into -1.
// ponytail: via ifstream, not <filesystem> (it needs macOS >= 10.15 and the wheels target 10.13).
long long file_size(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return -1;
    const std::streamoff size = in.tellg();
    if (size < 0) return -1;
    in.seekg(0);
    if (size > 0 && in.peek() == std::ifstream::traits_type::eof()) return -1;
    return static_cast<long long>(size);
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
    server::before_hook before;
    server::after_hook after;
    server::error_hook error;
    server::exception_hook exception;
    detail::response_compression compression; // off unless set_compression() was called
};

struct route_entry {
    std::string method;
    std::vector<std::string> segs; // ":name" segments capture
    handler h;          // exactly one of these four is set
    shared_handler sh;
    async_handler ah;
    io_handler ih;
};

// A request that is parsed (and maybe already matched to a route), on its way to be run.
struct prepared {
    std::shared_ptr<request> req;
    std::string raw_path;                // the path as sent, still percent-encoded
    const route_entry* route = nullptr;  // valid when `matched`; null = no route fits
    bool matched = false;
};

struct mount_entry {
    std::string prefix; // "/files" (no trailing slash, "" = root)
    std::string dir;
};

// True on the threads that run an engine (the I/O threads) while the server listens.
thread_local bool tl_on_io_thread = false;

// The way out of a request: (response, file offset, file length or -1 = all, what to release on the I/O thread afterwards).
// The last argument is optional for callers.
struct send_fn : std::function<void(response, long long, long long, std::shared_ptr<void>)> {
    using base = std::function<void(response, long long, long long, std::shared_ptr<void>)>;
    using base::base;
    void operator()(response res, long long offset, long long length, std::shared_ptr<void> keep = {}) const {
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
    void (*unwait)(void* list, std::uint64_t id) = nullptr; // forget this request (it no longer waits for an answer)
    std::atomic<bool> answered{false};
    std::atomic<bool> settled{false};
    void settle_once() {
        if (unwait && !settled.exchange(true)) unwait(wait.get(), wait_id);
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
    std::atomic<int> async_timeout{60};

    // Requests that wait for the answer of a route_async / route_io handler, oldest first. They all have
    // the same timeout, so they expire in the order they came in and ONE timer can watch them all. (A
    // timer per request, which this used to be, left hundreds of thousands of live timers under load,
    // and that alone cost more than the whole rest of the request.)
    struct waiting {
        struct entry {
            std::weak_ptr<responder::impl> state;
            std::shared_ptr<const request> req; // for the log line only; dropped when answered
            std::chrono::steady_clock::time_point deadline;
            int timeout_s;
        };
        std::mutex mu;
        std::map<std::uint64_t, entry> by_arrival; // key = arrival number
        std::uint64_t next = 0;
        bool armed = false; // a sweep is scheduled
    };
    std::shared_ptr<waiting> wait = std::make_shared<waiting>();

    // One engine per I/O thread. engines[0] is the one that runs on the thread that calls listen, owns the timers
    // (`engine`), and is the only one before set_io_threads() is called. Every setting that reaches an engine is
    // remembered in `applied` and replayed on engines made later.
    std::vector<std::unique_ptr<detail::async_server>> engines;
    detail::async_server* engine = nullptr; // engines[0]
    std::vector<std::function<void(detail::async_server&)>> applied;
    std::size_t max_conn_total = 0; // 0 = no limit; shared out over the engines
    std::atomic<detail::worker_pool*> pool{nullptr}; // alive only while listening
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
    static bool make_request(detail::http_request& hr, request& req, std::string& raw_path) {
        req.method = std::move(hr.method);
        req.body = std::move(hr.body);
        req.headers = std::move(hr.headers);
        std::string target = hr.target;
        const std::size_t hash = target.find('#');
        if (hash != std::string::npos) target.resize(hash);
        if (target.compare(0, 7, "http://") == 0 || target.compare(0, 8, "https://") == 0) { // absolute form
            const std::size_t slash = target.find('/', target.find("//") + 2);
            target = slash == std::string::npos ? "/" : target.substr(slash);
        }
        if (target.empty() || (target[0] != '/' && target != "*")) return false;
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
    detail::wire_response to_wire(response res, long long offset, long long length) const {
        detail::wire_response w;
        w.status = res.status > 0 ? res.status : 200;
        std::string content_type = "text/plain";
        for (auto& [name, value] : res.headers) {
            if (!header_ok(name, value)) return failure(500, "Internal Server Error"); // never send a split header
            if (ieq(name, "Content-Type")) content_type = value;
            else w.headers.emplace_back(name, value);
        }
        if (!header_ok("Content-Type", content_type)) return failure(500, "Internal Server Error");
        w.headers.emplace_back("Content-Type", content_type);

        if (!res.file.empty()) {
            const long long size = file_size(res.file);
            if (size < 0) return failure(404, "Not Found");
            if (offset > size) offset = size;
            const long long len = length < 0 ? size - offset : std::min(length, size - offset);
            w.length = len;
            auto in = std::make_shared<std::ifstream>(res.file, std::ios::binary);
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
    send_fn make_sender(const std::shared_ptr<request>& req, detail::async_server::answer_fn answer) {
        std::weak_ptr<config> weak = cfg;
        return [this, weak, req, answer = std::move(answer)](response res, long long offset, long long length, std::shared_ptr<void> keep) {
            auto c = weak.lock();
            if (!c) return; // the server is gone
            if ((c->error || c->after || c->compression.worth_a_thread(res)) && tl_on_io_thread) {
                // A route_io handler answered on the spot. The hooks may block (Python code waits for
                // the GIL) and compressing a big body takes a while; the I/O thread must never wait: a worker finishes the job.
                auto* p = pool.load();
                auto job = [this, weak, req, answer, res = std::move(res), offset, length, keep = std::move(keep)]() mutable {
                    make_sender(req, answer)(std::move(res), offset, length, std::move(keep));
                };
                if (p && p->enqueue(job)) return;
                // no pool (shutting down): carry on here rather than lose the answer
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
        };
    }

    void on_exception(const std::shared_ptr<request>& req, const send_fn& send, std::exception_ptr ep) {
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
        for (auto& part : parts) part = percent_decode(part, false);

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
    bool may_be_static(const request& req) const {
        if (req.method != "GET" && req.method != "HEAD") return false;
        for (const auto& m : mounts) if (under_prefix(req.path, m.prefix)) return true;
        return false;
    }

    // True when `file` (symlinks resolved) is still inside `dir` (resolved too). A file that does not
    // exist counts as inside: the caller's file_size() answers that case.
    static bool inside_dir(const std::string& file, const std::string& dir) {
#ifdef _WIN32
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path real = fs::canonical(fs::path(file), ec);
        if (ec) return true;
        const fs::path root = fs::canonical(fs::path(dir.empty() ? "." : dir), ec);
        if (ec) return false;
        const fs::path rel = real.lexically_relative(root);
        return !rel.empty() && *rel.begin() != "..";
#else
        // ponytail: realpath, not <filesystem> (it needs macOS >= 10.15 and the wheels target 10.13).
        char* f = ::realpath(file.c_str(), nullptr);
        if (!f) return true;
        std::string real(f);
        std::free(f);
        char* d = ::realpath(dir.empty() ? "." : dir.c_str(), nullptr);
        if (!d) return false;
        std::string root(d);
        std::free(d);
        if (root.size() > 1 && root.back() == '/') root.pop_back();
        if (root == "/") return true;
        return real == root || (real.size() > root.size() && real.compare(0, root.size(), root) == 0 && real[root.size()] == '/');
#endif
    }

    bool serve_static(const request& req, const send_fn& send) {
        for (const auto& m : mounts) {
            const std::string& p = req.path;
            if (!under_prefix(p, m.prefix)) continue;
            const std::string rel = p.substr(m.prefix.size()); // "" or "/a/b"
            if (rel.find('\\') != std::string::npos) continue;
            bool safe = true;
            for (const auto& seg : split_path(rel.empty() ? "/" : rel)) if (seg == "..") safe = false;
            if (!safe) continue; // never leave the mounted directory
            std::string full = m.dir;
            if (!full.empty() && full.back() != '/' && full.back() != '\\') full += '/';
            full += rel.empty() ? "" : rel.substr(1);
            if (rel.empty() || rel.back() == '/') full += "index.html";
            if (!inside_dir(full, m.dir)) continue; // a symlink must not lead out of the mounted directory
            const long long size = file_size(full);
            if (size < 0) continue;

            response res;
            res.status = 200;
            res.file = full;
            res.headers.emplace_back("Content-Type", mime_for(full));
            res.headers.emplace_back("Accept-Ranges", "bytes");
            long long start = 0, end = size - 1;
            switch (parse_range(req.header("Range"), size, start, end)) {
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
        return false;
    }

    // ---- the 504 backstop ----

    void track(const std::shared_ptr<responder::impl>& state, const std::shared_ptr<request>& req, int timeout_s) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
        std::uint64_t id;
        bool arm = false;
        auto w = wait;
        {
            std::lock_guard<std::mutex> lock(w->mu);
            id = w->next++;
            w->by_arrival.emplace_hint(w->by_arrival.end(), id, waiting::entry{state, req, deadline, timeout_s});
            if (!w->armed) w->armed = arm = true;
        }
        // erasing an id that a sweep already took out is harmless
        state->wait = w;
        state->wait_id = id;
        state->unwait = [](void* list, std::uint64_t at) {
            auto* l = static_cast<waiting*>(list);
            std::lock_guard<std::mutex> lock(l->mu);
            l->by_arrival.erase(at);
        };
        if (arm) arm_sweep(deadline);
    }

    void arm_sweep(std::chrono::steady_clock::time_point at) {
        auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(at - std::chrono::steady_clock::now());
        if (delay < std::chrono::milliseconds(10)) delay = std::chrono::milliseconds(10);
        engine->after(delay, [this] { sweep(); });
    }

    // On the loop thread. Takes the expired requests out of the queue and answers them 504 (on a worker:
    // the hooks and the write may block), then schedules the next sweep for the oldest one still waiting.
    void sweep() {
        std::vector<std::pair<std::shared_ptr<responder::impl>, waiting::entry>> late;
        bool more = false;
        std::chrono::steady_clock::time_point next;
        {
            std::lock_guard<std::mutex> lock(wait->mu);
            const auto now = std::chrono::steady_clock::now();
            auto it = wait->by_arrival.begin();
            for (; it != wait->by_arrival.end() && it->second.deadline <= now; ++it) {
                if (auto st = it->second.state.lock()) late.emplace_back(std::move(st), std::move(it->second));
            }
            wait->by_arrival.erase(wait->by_arrival.begin(), it);
            more = !wait->by_arrival.empty();
            if (more) next = wait->by_arrival.begin()->second.deadline;
            wait->armed = more;
        }
        if (more) arm_sweep(next);
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
        pr.req = std::make_shared<request>();
        if (!make_request(hr, *pr.req, pr.raw_path)) {
            HTTPP_LOG(debug) << "bad request target: " << hr.target.substr(0, 100);
            answer(failure(400, "Bad Request"));
            return true;
        }
        if (!may_be_static(*pr.req)) { // else the worker looks at the disk first, then matches
            const std::string method = pr.req->method == "HEAD" ? "GET" : pr.req->method; // HEAD runs the GET route
            pr.route = match(method, pr.raw_path, pr.req->path_params);
            pr.matched = true;
            if (pr.route && pr.route->ih) { // right here: such a handler must not block
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
        pr.req = std::make_shared<request>();
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
        const std::shared_ptr<request>& req = pr.req;
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

        if ((req->method == "GET" || req->method == "HEAD") && serve_static(*req, send)) return;

        const route_entry* r = pr.route;
        if (!pr.matched) {
            const std::string method = req->method == "HEAD" ? "GET" : req->method; // HEAD runs the GET route; the engine drops the body
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

        // If nobody answers in time, answer 504 ourselves (see `waiting`).
        const int timeout = async_timeout.load();
        if (timeout > 0) track(state, req, timeout);

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
    }

    void add_route(const std::string& method, const std::string& path, route_entry r) {
        std::string m = method;
        std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return std::toupper(c); });
        if (m != "GET" && m != "POST" && m != "PUT" && m != "PATCH" && m != "DELETE" && m != "OPTIONS")
            throw std::invalid_argument("httpp::server::route: unsupported method '" + method + "'");
        r.method = std::move(m);
        r.segs = split_path(!path.empty() && path[0] == '/' ? path : "/" + path);
        if (r.ih) ++io_route_count;
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

server& server::before(before_hook hook) { impl_->cfg->before = std::move(hook); return *this; }
server& server::after(after_hook hook) { impl_->cfg->after = std::move(hook); return *this; }
server& server::set_error_handler(error_hook hook) { impl_->cfg->error = std::move(hook); return *this; }
server& server::set_exception_handler(exception_hook hook) { impl_->cfg->exception = std::move(hook); return *this; }

server& server::set_thread_pool(std::size_t base, std::size_t max) {
    if (base == 0) base = 1;
    if (max < base) max = base * 4;
    impl_->pool_base = base;
    impl_->pool_max = max;
    return *this;
}
server& server::set_read_timeout(int seconds) { impl_->configure([seconds](detail::async_server& e) { e.set_read_timeout(seconds); }); return *this; }
server& server::set_write_timeout(int seconds) { impl_->configure([seconds](detail::async_server& e) { e.set_write_timeout(seconds); }); return *this; }
server& server::set_keep_alive_timeout(int seconds) { impl_->configure([seconds](detail::async_server& e) { e.set_keep_alive_timeout(seconds); }); return *this; }
server& server::set_keep_alive_max(std::size_t requests) { impl_->configure([requests](detail::async_server& e) { e.set_keep_alive_max(requests); }); return *this; }
server& server::set_max_body(std::size_t bytes) { impl_->configure([bytes](detail::async_server& e) { e.set_max_body(bytes); }); return *this; }
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
    std::string prefix = mount_path;
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back(); // "/" -> "" (the root)
    if (!prefix.empty() && prefix[0] != '/') prefix = "/" + prefix;
    impl_->mounts.push_back({std::move(prefix), local_dir});
}

int server::bind_to_any_port(const std::string& host) {
    const int port = impl_->bind_all(host, 0);
    if (port >= 0) { HTTPP_LOG(info) << "listening on http://" << host << ':' << port; }
    return port;
}

bool server::listen_after_bind() {
    const std::size_t base = impl_->pool_base;
    const std::size_t max = impl_->pool_max ? impl_->pool_max : base * 4;
    detail::worker_pool pool(base, max);
    impl_->pool = &pool;
    {
        std::lock_guard<std::mutex> lock(impl_->wait->mu); // a previous run's timer died with its loop
        impl_->wait->by_arrival.clear();
        impl_->wait->armed = false;
    }
    // The other I/O threads; this thread runs engines[0] (and owns the timers).
    std::vector<std::thread> more;
    std::atomic<bool> others_ok{true};
    for (std::size_t i = 1; i < impl_->engines.size(); ++i) {
        detail::async_server* e = impl_->engines[i].get();
        more.emplace_back([e, &others_ok] {
            tl_on_io_thread = true;
            if (!e->run()) others_ok = false;
            tl_on_io_thread = false;
        });
    }
    tl_on_io_thread = true; // the engine's loop runs on this very thread
    const bool ok = impl_->engines[0]->run(); // returns after stop()
    tl_on_io_thread = false;
    for (auto& e : impl_->engines) e->stop(); // if this one ended on its own, the others must end too
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
    HTTPP_LOG(info) << "listening on http://" << host << ':' << bound;  // after the bind, so a taken port never says it
    return listen_after_bind();
}

void server::stop() { for (auto& e : impl_->engines) e->stop(); }

} // namespace httpp
