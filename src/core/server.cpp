#include "httpp/server.hpp"

// httpp::server on top of the async engine (internal/async_server.hpp: Asio + llhttp). Routing,
// static files, hooks and response building live here; the engine only moves bytes. No
// cpp-httplib: this translation unit does not include it.

#include "internal/async_server.hpp"
#include "internal/worker_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace httpp {
namespace {

// ---------------------------------------------------------------- small helpers

bool ieq(const std::string& a, const char* b) {
    const std::size_t n = std::strlen(b);
    return a.size() == n && std::equal(a.begin(), a.end(), b, [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// %XX decoding; a malformed escape is kept as is. `plus` turns '+' into a space (query strings).
std::string percent_decode(const std::string& in, bool plus) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '%' && i + 2 < in.size() + 0 && hex_value(in[i + 1]) >= 0 && hex_value(in[i + 2]) >= 0) {
            out += static_cast<char>(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2]));
            i += 2;
        } else if (c == '+' && plus) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

// "a=1&b=%20&c" -> [(a,1),(b," "),(c,"")], appended to `out`.
void parse_query(const std::string& q, std::vector<std::pair<std::string, std::string>>& out) {
    std::size_t pos = 0;
    while (pos <= q.size()) {
        std::size_t end = q.find('&', pos);
        if (end == std::string::npos) end = q.size();
        if (end > pos) {
            const std::string item = q.substr(pos, end - pos);
            const std::size_t eq = item.find('=');
            if (eq == std::string::npos) out.emplace_back(percent_decode(item, true), "");
            else out.emplace_back(percent_decode(item.substr(0, eq), true), percent_decode(item.substr(eq + 1), true));
        }
        pos = end + 1;
    }
}

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

// ---------------------------------------------------------------- the server

struct config { // read-only once listening; shared with answers that may outlive the server
    server::before_hook before;
    server::after_hook after;
    server::error_hook error;
    server::exception_hook exception;
};

struct route_entry {
    std::string method;
    std::vector<std::string> segs; // ":name" segments capture
    handler h;
    async_handler ah;
};

struct mount_entry {
    std::string prefix; // "/files" (no trailing slash, "" = root)
    std::string dir;
};

using send_fn = std::function<void(response, long long /*file offset*/, long long /*file length, -1 = all*/)>;

} // namespace

struct responder::impl {
    std::function<void(response)> sender;
    std::function<void()> abandon;
    std::atomic<bool> answered{false};
    ~impl() {
        if (!answered.load() && abandon) abandon(); // every copy dropped without an answer: close, say nothing
    }
};

void responder::send(response res) const {
    if (!impl_ || impl_->answered.exchange(true)) return; // only the first answer counts
    impl_->sender(std::move(res));
}

struct server::impl {
    std::shared_ptr<config> cfg = std::make_shared<config>();
    std::vector<route_entry> routes;
    std::vector<mount_entry> mounts;
    std::size_t pool_base = std::max<std::size_t>(8, std::thread::hardware_concurrency());
    std::size_t pool_max = 0; // 0 = 4 x base
    std::atomic<int> async_timeout{60};
    std::unique_ptr<detail::async_server> engine;
    std::atomic<detail::worker_pool*> pool{nullptr}; // alive only while listening

    impl() {
        engine = std::make_unique<detail::async_server>(
            detail::async_server::options{},
            [this](detail::http_request r, detail::async_server::answer_fn answer) {
                auto* p = pool.load();
                return p && p->enqueue([this, r = std::move(r), answer = std::move(answer)]() mutable {
                    handle(std::move(r), std::move(answer));
                });
            },
            [this](std::function<void()> job) {
                auto* p = pool.load();
                return p && p->enqueue(std::move(job));
            });
    }

    // ---- request ----
    static bool make_request(const detail::http_request& hr, request& req, std::string& raw_path) {
        req.method = hr.method;
        req.body = hr.body;
        req.headers = hr.headers;
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
        return [this, weak, req, answer = std::move(answer)](response res, long long offset, long long length) {
            auto c = weak.lock();
            if (!c) return; // the server is gone
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
                    } catch (...) {} // a broken error handler keeps the plain error
                }
                if (c->after) {
                    response out; // status and headers only: the body may be huge or streamed
                    out.status = res.status;
                    out.headers = res.headers;
                    c->after(*req, out);
                    res.status = out.status;
                    res.headers = std::move(out.headers);
                }
                w = to_wire(std::move(res), offset, length);
            } catch (...) {
                w = failure(500, "Internal Server Error");
            }
            answer(std::move(w));
        };
    }

    void on_exception(const std::shared_ptr<request>& req, const send_fn& send, std::exception_ptr ep) {
        response out;
        out.status = 500;
        out.body = "Internal Server Error";
        if (cfg->exception) {
            try { cfg->exception(*req, out, ep); } catch (...) {}
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
        for (const auto& r : routes) {
            if (r.method != method || r.segs.size() != parts.size()) continue;
            std::vector<std::pair<std::string, std::string>> found;
            bool ok = true;
            for (std::size_t i = 0; i < parts.size() && ok; ++i) {
                const std::string& seg = r.segs[i];
                if (seg.size() > 1 && seg[0] == ':') {
                    if (parts[i].empty()) ok = false;
                    else found.emplace_back(seg.substr(1), parts[i]);
                } else {
                    ok = seg == parts[i];
                }
            }
            if (ok) {
                params = std::move(found);
                return &r;
            }
        }
        return nullptr;
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

    bool serve_static(const request& req, const send_fn& send) {
        for (const auto& m : mounts) {
            const std::string& p = req.path;
            if (p.compare(0, m.prefix.size(), m.prefix) != 0) continue;
            if (p.size() > m.prefix.size() && p[m.prefix.size()] != '/') continue; // "/filesX" is not under "/files"
            const std::string rel = p.substr(m.prefix.size()); // "" or "/a/b"
            if (rel.find('\\') != std::string::npos) continue;
            bool safe = true;
            for (const auto& seg : split_path(rel.empty() ? "/" : rel)) if (seg == "..") safe = false;
            if (!safe) continue; // never leave the mounted directory
            std::string full = m.dir;
            if (!full.empty() && full.back() != '/' && full.back() != '\\') full += '/';
            full += rel.empty() ? "" : rel.substr(1);
            if (rel.empty() || rel.back() == '/') full += "index.html";
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

    // Runs on a pool thread.
    void handle(detail::http_request hr, detail::async_server::answer_fn answer) {
        auto req = std::make_shared<request>();
        std::string raw_path;
        if (!make_request(hr, *req, raw_path)) return answer(failure(400, "Bad Request"));
        const send_fn send = make_sender(req, answer);

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

        const std::string method = req->method == "HEAD" ? "GET" : req->method; // HEAD runs the GET route; the engine drops the body
        const route_entry* r = match(method, raw_path, req->path_params);
        if (!r) {
            response nf;
            nf.status = 404;
            return send(std::move(nf), 0, -1);
        }

        if (!r->ah) {
            response res;
            res.status = 200;
            try {
                r->h(*req, res);
            } catch (...) {
                return on_exception(req, send, std::current_exception());
            }
            return send(std::move(res), 0, -1);
        }

        // Asynchronous route: the handler may return at once; whoever holds the responder answers later.
        auto state = std::make_shared<responder::impl>();
        state->sender = [send](response res) { send(std::move(res), 0, -1); };
        state->abandon = [answer] {
            detail::wire_response none;
            none.status = 0; // close without an answer
            answer(std::move(none));
        };
        responder rsp;
        rsp.impl_ = state;

        // If nobody answers in time, answer 504 ourselves. The timer only holds a weak reference, so a
        // response that was sent (and dropped) in the meantime costs nothing.
        const int timeout = async_timeout.load();
        if (timeout > 0) {
            std::weak_ptr<responder::impl> weak = state;
            engine->after(std::chrono::seconds(timeout), [this, weak] {
                auto strong = weak.lock();
                if (!strong) return;
                auto answer_504 = [strong] {
                    responder late;
                    late.impl_ = strong;
                    response err;
                    err.status = 504;
                    err.body = "Gateway Timeout";
                    late.send(std::move(err)); // no-op if it was answered after all
                };
                // Hooks and the write may block: do it on a worker, not on the loop thread.
                if (auto* p = pool.load(); !p || !p->enqueue(answer_504)) answer_504();
            });
        }

        try {
            r->ah(*req, rsp);
        } catch (...) {
            response err;
            err.status = 500;
            err.body = "Internal Server Error";
            if (cfg->exception) {
                try { cfg->exception(*req, err, std::current_exception()); } catch (...) {}
            }
            rsp.send(std::move(err)); // no-op if the handler had already answered
        }
    }

    void add_route(const std::string& method, const std::string& path, handler h, async_handler ah) {
        std::string m = method;
        std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return std::toupper(c); });
        if (m != "GET" && m != "POST" && m != "PUT" && m != "PATCH" && m != "DELETE" && m != "OPTIONS")
            throw std::invalid_argument("httpp::server::route: unsupported method '" + method + "'");
        route_entry r;
        r.method = std::move(m);
        r.segs = split_path(!path.empty() && path[0] == '/' ? path : "/" + path);
        r.h = std::move(h);
        r.ah = std::move(ah);
        routes.push_back(std::move(r));
    }
};

server::server() : impl_(std::make_unique<impl>()) {}
server::~server() = default;
server::server(server&&) noexcept = default;
server& server::operator=(server&&) noexcept = default;

void server::route(const std::string& method, const std::string& path, handler h) {
    impl_->add_route(method, path, std::move(h), nullptr);
}
void server::route_async(const std::string& method, const std::string& path, async_handler h) {
    // An empty `handler` marks nothing here: the async handler is what makes the route async.
    impl_->add_route(method, path, nullptr, std::move(h));
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
server& server::set_read_timeout(int seconds) { impl_->engine->set_read_timeout(seconds); return *this; }
server& server::set_write_timeout(int seconds) { impl_->engine->set_write_timeout(seconds); return *this; }
server& server::set_keep_alive_timeout(int seconds) { impl_->engine->set_keep_alive_timeout(seconds); return *this; }
server& server::set_keep_alive_max(std::size_t requests) { impl_->engine->set_keep_alive_max(requests); return *this; }
server& server::set_max_body(std::size_t bytes) { impl_->engine->set_max_body(bytes); return *this; }
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

int server::bind_to_any_port(const std::string& host) { return impl_->engine->bind(host, 0); }

bool server::listen_after_bind() {
    const std::size_t base = impl_->pool_base;
    const std::size_t max = impl_->pool_max ? impl_->pool_max : base * 4;
    detail::worker_pool pool(base, max);
    impl_->pool = &pool;
    const bool ok = impl_->engine->run(); // returns after stop()
    impl_->pool = nullptr;
    pool.shutdown(); // joins the workers
    return ok;
}

bool server::listen(const std::string& host, int port) {
    if (impl_->engine->bind(host, port) < 0) return false;
    return listen_after_bind();
}

void server::stop() { impl_->engine->stop(); }

} // namespace httpp
