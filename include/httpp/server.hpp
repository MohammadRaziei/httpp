#pragma once

#include "httpp/export.hpp"
#include "httpp/path.hpp"
#include "httpp/route_pattern.hpp"
#include "httpp/client.hpp" // reuses httpp::response for the outgoing side too
#include "httpp/websocket.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace httpp {

class router; // router.hpp

// One uploaded file of a multipart/form-data body.
struct upload_file {
    std::string field;         // the form field name
    std::string filename;      // as sent, without any directory part; "" when the browser sent an empty file input
    std::string content_type;  // as sent; "application/octet-stream" when the part had none
    std::string data;
};

// The decoded form of a request, see request::form().
struct form_data {
    std::vector<std::pair<std::string, std::string>> fields;  // in wire order, repeated names kept
    std::vector<upload_file> files;                           // in wire order

    // First value of a field, "" if absent.
    std::string field(const std::string& name) const {
        for (const auto& [k, v] : fields) if (k == name) return v;
        return "";
    }
    // First file sent under this field name, nullptr if none.
    const upload_file* file(const std::string& name) const {
        for (const auto& f : files) if (f.field == name) return &f;
        return nullptr;
    }
};

// Minimal per-request info handed to a route handler.
struct server_request {
    std::string method;
    std::string path;
    std::string body;
    // Repeated names are kept (in wire order), like response::headers.
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::pair<std::string, std::string>> query;        // ?a=1&b=2
    std::vector<std::pair<std::string, std::string>> path_params;  // "/users/{id}"
    bool secure = false;  // the connection is TLS (https, wss): the server was given set_tls()
    std::string scheme() const { return secure ? "https" : "http"; }
    // The address of the client as text, "" when unknown. It is the address of the connection, unless a forwarded_headers
    // middleware (httpp/middleware.hpp) trusts the connection's address and takes the client's from X-Forwarded-For (ADR-0069).
    std::string client_ip;

    // First match, "" if absent. Header names are case-insensitive.
    std::string header(const std::string& name) const {
        for (const auto& [k, v] : headers) {
            if (k.size() == name.size() &&
                std::equal(k.begin(), k.end(), name.begin(), [](unsigned char a, unsigned char b) {
                    return std::tolower(a) == std::tolower(b);
                }))
                return v;
        }
        return "";
    }
    std::string query_param(const std::string& name) const {
        for (const auto& [k, v] : query) if (k == name) return v;
        return "";
    }
    std::string path_param(const std::string& name) const {
        for (const auto& [k, v] : path_params) if (k == name) return v;
        return "";
    }
    // The same, converted: req.path_param<int>("id"). Throws std::invalid_argument when the parameter is absent or
    // does not read as a T. A route written {id:int} (see route_pattern.hpp) only matches when it does, so for
    // those the conversion cannot fail (it can still overflow a type narrower than 64 bits).
    template <class T>
    T path_param(const std::string& name) const {
        static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, char> && !std::is_same_v<T, bool>,
                      "path_param<T>: T must be an integer or floating-point type");
        const std::string v = path_param(name);
        T out{};
        std::istringstream in(v);
        in.imbue(std::locale::classic());
        if (v.empty() || !(in >> out) || in.peek() != std::char_traits<char>::eof())
            throw std::invalid_argument("path parameter '" + name + "' is missing or not a valid number: '" + v + "'");
        return out;
    }
    // The body as a form: application/x-www-form-urlencoded (fields only) or multipart/form-data (fields and files).
    // Any other Content-Type gives an empty form. Parsed on every call, so call it once. A malformed multipart body
    // (no boundary, a part that never ends, a part without a name, more than 1024 parts, a part header over 8 KiB)
    // throws std::invalid_argument: answer it with a 400. The whole body is already in memory (see set_max_body).
    HTTPP_API form_data form() const;
    // From the Cookie header ("a=1; b=2"); "" if absent.
    std::string cookie(const std::string& name) const {
        const std::string all = header("Cookie");
        std::size_t pos = 0;
        while (pos < all.size()) {
            std::size_t end = all.find(';', pos);
            if (end == std::string::npos) end = all.size();
            std::size_t start = pos;
            while (start < end && all[start] == ' ') start++;
            const std::size_t eq = all.find('=', start);
            if (eq != std::string::npos && eq < end && all.compare(start, eq - start, name) == 0)
                return all.substr(eq + 1, end - eq - 1);
            pos = end + 1;
        }
        return "";
    }
};

using handler = std::function<void(const server_request&, response&)>;

// Asynchronous ("deferred") responses. An async handler gets a `responder`
// instead of a response to fill in. It may return immediately: the worker
// thread is released at once, the connection stays open, and the response is
// sent whenever `responder::send` is called, from any thread. This is what lets
// an event loop answer many requests at once without one blocked thread each.
//
//  - send() is thread-safe; only the first call has any effect, and it may
//    happen before the handler has returned.
//  - On POSIX the connection is kept alive after an async response (if the
//    client allows it); on Windows it is closed after each async response.
//  - If the server is destroyed first, pending connections are closed and a
//    later send() does nothing. Dropping every copy of a responder without
//    calling send() closes the connection without a response.
class responder {
public:
    HTTPP_API void send(response res) const;

private:
    friend class server;
    friend struct detail::ws_upgrade;  // a pending WebSocket upgrade answers through the same state
    struct impl;
    std::shared_ptr<impl> impl_;
};

using async_handler = std::function<void(const server_request&, responder)>;

// Handlers that get their request by shared pointer, so they can keep it (hand it to another
// thread, queue it) without copying it.
//  - shared_handler: a normal handler (runs on a worker thread, may block) that fills in a response.
//  - io_handler: runs directly on the single I/O thread that reads and parses requests, answers
//    through a responder like an async handler. It MUST return at once and never block, never take
//    a lock someone may hold for long, never wait for a thread that could be waiting for the I/O
//    thread. It exists to hand the request to an event loop that lives elsewhere (the Python
//    asyncio loop is one), skipping the extra thread hop an async_handler costs.
using shared_handler = std::function<void(std::shared_ptr<const server_request>, response&)>;
using io_handler = std::function<void(std::shared_ptr<const server_request>, responder)>;

// httpp::server is what replaces "spin up libcurl/httplib yourself" for the
// server side: register routes or serve a directory (the httpp CLI's
// `httpp server` command, analogous to `python -m http.server`, is a thin
// wrapper around this class). It runs on our own Asio + llhttp engine (see
// src/core/server.cpp); that engine never appears in this public header.
class directory;
class forwarded_headers;
class server_stopper;

class server {
public:
    // A `server` is a handle (ADR-0081): copying it gives a second handle to the SAME server, with the same routes, settings and
    // sockets, so mounting or configuring through any handle changes all of them. The server closes when the last handle is
    // destroyed (or by stop()/shutdown()). A handler must not capture a `server` (that is a cycle: the server keeps the handler,
    // the handler keeps the server, and it is never freed); capture stopper() instead. A moved-from handle can only be assigned
    // to or destroyed.
    HTTPP_API server();
    HTTPP_API ~server();

    HTTPP_API server(const server&) noexcept;
    HTTPP_API server(server&&) noexcept;
    HTTPP_API server& operator=(const server&) noexcept;
    HTTPP_API server& operator=(server&&) noexcept;

    // True when both handles are the same server (handles are copied, so their addresses say nothing).
    HTTPP_API bool same_as(const server& other) const noexcept;
    // A handle that can stop this server but does not keep it alive; see server_stopper.
    HTTPP_API server_stopper stopper() const;

    // `path` may contain {name} or {name:int|float} segments ("/users/{id}"), captured into
    // request::path_params. `method` is GET/POST/PUT/PATCH/DELETE/OPTIONS
    // (case-insensitive); anything else throws std::invalid_argument.
    // A handler may set a "Content-Type" entry in response::headers
    // (default: text/plain).
    HTTPP_API void route(const std::string& method, const std::string& path, handler h);
    // Shorthands for route("<METHOD>", ...). `del` because `delete` is a keyword.
    HTTPP_API void get(const std::string& path, handler h);
    HTTPP_API void post(const std::string& path, handler h);
    HTTPP_API void put(const std::string& path, handler h);
    HTTPP_API void patch(const std::string& path, handler h);
    HTTPP_API void del(const std::string& path, handler h);
    // Same as route(), but the handler answers through a `responder` (see above).
    // An exception thrown by the handler is answered with a plain 500 (or goes to the exception
    // handler, see set_exception_handler). If no answer is sent within set_async_timeout() seconds
    // (default 60), the client gets a 504 and a later send() does nothing.
    HTTPP_API void route_async(const std::string& method, const std::string& path, async_handler h);
    HTTPP_API void get_async(const std::string& path, async_handler h);
    HTTPP_API void post_async(const std::string& path, async_handler h);
    HTTPP_API void put_async(const std::string& path, async_handler h);
    HTTPP_API void patch_async(const std::string& path, async_handler h);
    HTTPP_API void del_async(const std::string& path, async_handler h);
    // Like route(), but the handler gets the request by shared pointer (see shared_handler).
    HTTPP_API void route_shared(const std::string& method, const std::string& path, shared_handler h);
    // Like route_async(), but the handler runs on the I/O thread (see io_handler). Everything else
    // is the same: the 504 timeout, the exception handler, the hooks, a late send() from any thread.
    // A before hook (it may block) or a mounted directory that has the file still runs first, on a
    // worker thread, and then the handler runs there instead.
    HTTPP_API void route_io(const std::string& method, const std::string& path, io_handler h);
    // A WebSocket endpoint (RFC 6455): GET `path` ({name} segments work as in route()) upgrades the connection and the callbacks of `h`
    // then run on its I/O thread, see ws_handler and websocket. A request that is not a valid upgrade gets 400 (426 for a Sec-WebSocket-Version
    // other than 13). The before hook runs first (and may refuse); the after hook does not run for the 101 answer. Needs no worker thread.
    HTTPP_API void route_ws(const std::string& path, ws_handler h);
    // Registers every route of `r` under `prefix` + r.prefix() (see router.hpp). The router is not kept.
    HTTPP_API void include(const router& r, const std::string& prefix = "");
    // Serve the files of `local_dir` under `mount_path` with the defaults of `directory` (see below); `directory(local_dir)...mount(s, path)`
    // is the same call with every setting open.
    HTTPP_API void serve_directory(const std::string& mount_path, const std::string& local_dir);

    HTTPP_API int bind_to_any_port(const std::string& host);
    // Block until stop(). Return false if the socket could not be bound/listened on
    // (e.g. port already in use), true after a normal stop().
    HTTPP_API bool listen_after_bind();
    // Bind the socket now and listen later (listen_after_bind, listen_until_signal): url() is known in between. Returns false when
    // it cannot be bound (the port is in use); listen(host, port) is bind() then listen_after_bind().
    HTTPP_API bool bind(const std::string& host, int port);
    HTTPP_API bool listen(const std::string& host, int port);
    // The address the server is bound to, "https://127.0.0.1:8000/" (https after set_tls, an IPv6 host in brackets, the port that was
    // really bound), or "" before a bind. The host is the one that was asked for: "0.0.0.0" stays "0.0.0.0".
    HTTPP_API std::string url() const;
    // After bind(): listen until SIGINT or SIGTERM. The first signal is shutdown(grace), a second one is stop(). It replaces the
    // process's SIGINT and SIGTERM handlers while it runs and puts them back before it returns, so call it from one place (one server)
    // at a time. Returns false when it could not listen, true after a normal stop.
    HTTPP_API bool listen_until_signal(std::chrono::milliseconds grace = std::chrono::seconds(10));
    HTTPP_API void stop();
    // Graceful stop (ADR-0038), from any thread; listen() returns once it is done. The listener closes at once (new connections
    // are refused), idle keep-alive connections and WebSocket connections are closed at once, and a request that is being
    // handled gets its answer, with "Connection: close", before its connection ends. Whatever is still open after `grace`
    // (a handler that never answers, a slow client) is cut as stop() does. stop() stays the immediate one and may be called
    // during a drain to end it. Does nothing when the server is not listening.
    HTTPP_API void shutdown(std::chrono::milliseconds grace = std::chrono::seconds(10));

    // --- Hooks. They run for every request, including static files and unmatched paths. ---
    // before: return true to answer the request yourself (the route is then not called), e.g. to
    //   reject it. The response starts empty with status 0 (sent as 200 unless you set it).
    // after: may change the response's status and headers on the way out (the body is not passed
    //   and changes to it are ignored). It also runs for async responses, when they are sent.
    // error handler: fill in the body for a 4xx/5xx the server produced itself (404, 405, ...).
    // exception handler: called when a route handler throws; the response starts as a 500.
    using before_hook = std::function<bool(const server_request&, response&)>;
    using after_hook = std::function<void(const server_request&, response&)>;
    using error_hook = std::function<void(const server_request&, response&)>;
    using exception_hook = std::function<void(const server_request&, response&, std::exception_ptr)>;
    HTTPP_API server& before(before_hook hook) &;
    server before(before_hook hook) && { before(std::move(hook)); return std::move(*this); }
    HTTPP_API server& after(after_hook hook) &;
    server after(after_hook hook) && { after(std::move(hook)); return std::move(*this); }
    HTTPP_API server& set_error_handler(error_hook hook) &;
    server set_error_handler(error_hook hook) && { set_error_handler(std::move(hook)); return std::move(*this); }
    HTTPP_API server& set_exception_handler(exception_hook hook) &;
    server set_exception_handler(exception_hook hook) && { set_exception_handler(std::move(hook)); return std::move(*this); }
    // TLS (ADR-0040): serve https and wss instead of http and ws, with a certificate chain (PEM, leaf first) and its private key (PEM,
    // `key_password` when it is encrypted). Files are read now: a missing or unreadable file, bad PEM, a wrong password or a key that does
    // not belong to the certificate throws std::runtime_error naming the file. TLS 1.2 and 1.3, no client certificates, no session
    // resumption. Call it before listening; the files are not watched (set it again, or restart, after renewing a certificate).
    // One port serves either TLS or plain HTTP, never both: a plain request to a TLS port is dropped.
    HTTPP_API server& set_tls(const path& cert_file, const path& key_file, const std::string& key_password = "") &;
    server set_tls(const path& cert_file, const path& key_file, const std::string& key_password = "") && { set_tls(cert_file, key_file, key_password); return std::move(*this); }
    // Middleware (ADR-0037): a before and an after hook that sit next to the application's own, so any number
    // can be added (see httpp/middleware.hpp for the ready-made ones). Either may be empty. Before hooks run in
    // the order of the use() calls, then the application's before(); the first that returns true answers and
    // the rest are skipped. After hooks run for every answer (also the ones a before hook gave): the
    // application's after() first, then the middleware in the reverse order. before() and after() never
    // remove middleware. Call it before listening.
    HTTPP_API server& use(before_hook before, after_hook after = {}) &;
    server use(before_hook before, after_hook after = {}) && { use(std::move(before), std::move(after)); return std::move(*this); }

    // --- Settings; call them before listening. Each returns *this. ---
    // Worker threads for sync handlers: `base` are kept, up to `max` are used under load
    // (default max = 4 x base; default pool: about one thread per core, at least 8). A job that has waited
    // `grow_after` for a free thread makes the pool grow by one: short means blocking handlers get their thread
    // at once, long keeps CPU-bound handlers on few threads (more of them only fight over a lock, such as
    // Python's GIL).
    HTTPP_API server& set_thread_pool(std::size_t base, std::size_t max = 0,
                                      std::chrono::microseconds grow_after = std::chrono::microseconds(500)) &;
    server set_thread_pool(std::size_t base, std::size_t max = 0,
                           std::chrono::microseconds grow_after = std::chrono::microseconds(500)) && {
        set_thread_pool(base, max, grow_after);
        return std::move(*this);
    }
    HTTPP_API server& set_read_timeout(int seconds) &;        // reading a request (default 5)
    server set_read_timeout(int seconds) && { set_read_timeout(seconds); return std::move(*this); }
    HTTPP_API server& set_write_timeout(int seconds) &;       // writing a response (default 5)
    server set_write_timeout(int seconds) && { set_write_timeout(seconds); return std::move(*this); }
    HTTPP_API server& set_keep_alive_timeout(int seconds) &;  // idle keep-alive connection (default 5)
    server set_keep_alive_timeout(int seconds) && { set_keep_alive_timeout(seconds); return std::move(*this); }
    HTTPP_API server& set_keep_alive_max(std::size_t requests) &; // requests per connection (default 1000, 0 = no limit)
    server set_keep_alive_max(std::size_t requests) && { set_keep_alive_max(requests); return std::move(*this); }
    HTTPP_API server& set_max_body(std::size_t bytes) &;      // largest request body accepted; 0 = no limit
    server set_max_body(std::size_t bytes) && { set_max_body(bytes); return std::move(*this); }
    HTTPP_API server& set_header_timeout(int seconds) &;      // whole request head must arrive within this (slowloris); default 10, 0 = no limit
    server set_header_timeout(int seconds) && { set_header_timeout(seconds); return std::move(*this); }
    HTTPP_API server& set_body_timeout(int seconds) &;        // whole request body must arrive within this after its head (slow drip); default 120, 0 = no limit
    server set_body_timeout(int seconds) && { set_body_timeout(seconds); return std::move(*this); }
    HTTPP_API server& set_max_connections(std::size_t n) &;   // open connections allowed; one more gets a 503; default 0 = no limit
    server set_max_connections(std::size_t n) && { set_max_connections(n); return std::move(*this); }
    HTTPP_API server& set_async_timeout(int seconds) &;       // route_async: answer within this or 504; 0 = never
    server set_async_timeout(int seconds) && { set_async_timeout(seconds); return std::move(*this); }
    // Gzip text answers (text/*, JSON, XML, JavaScript, SVG) of at least `min_size` bytes when the client sends
    // Accept-Encoding with gzip. Off by default; min_size 0 turns it off. `level` is zlib's 1 (fastest, default)
    // to 9 (smallest), else std::invalid_argument. Files, streamed bodies, partial answers and bodies that
    // already have a Content-Encoding are never touched. Compressing a body takes CPU time on the thread that
    // answers, so use a larger min_size or a lower level when answers are big and latency matters.
    HTTPP_API server& set_compression(std::size_t min_size, int level = 1) &;
    server set_compression(std::size_t min_size, int level = 1) && { set_compression(min_size, level); return std::move(*this); }
    // Let several processes listen on the same port, the kernel balancing connections between them
    // (SO_REUSEPORT: Linux only; elsewhere listening fails). Off by default, because it also lets an
    // unrelated process of the same user bind the port silently.
    HTTPP_API server& set_reuse_port(bool on) &;
    server set_reuse_port(bool on) && { set_reuse_port(on); return std::move(*this); }
    // How many I/O threads read, parse and answer (default 1). Each runs its own event loop and its own
    // acceptor on the same port (SO_REUSEPORT), the kernel spreads new connections over them, and a
    // connection stays on its thread for good, so the requests of one connection never overlap. route_io
    // handlers run on these threads. Needs SO_REUSEPORT (Linux, macOS, BSD): where it is missing, or the bind
    // fails, the server keeps one thread. Setting it turns reuse_port on (see above for the catch).
    // set_max_connections is then a total, shared out evenly. Call before listening.
    HTTPP_API server& set_io_threads(std::size_t n) &;
    server set_io_threads(std::size_t n) && { set_io_threads(n); return std::move(*this); }

private:
    friend class directory;
    friend class forwarded_headers;
    friend class server_stopper;
    // Runs on every request right after it is parsed and before any hook, on the thread that parses it; one per server (a second replaces it).
    void set_request_rewrite(std::function<void(server_request&)> rewrite);
    struct impl;
    std::shared_ptr<impl> impl_;
};

// What a route handler (or a signal watcher, or another thread) holds to stop a server without keeping it alive (ADR-0081). It is a
// weak reference: once the last `server` handle is gone, stop() and shutdown() do nothing and alive() is false. Capture this in a
// handler, never a `server`: a handler that holds a `server` makes a cycle (server -> handler -> server) and the server is never freed.
//     auto srv = httpp::server();
//     srv.get("/quit", [stop = srv.stopper()](const httpp::server_request&, httpp::response& res) { stop.shutdown(); res.body = "bye"; });
class server_stopper {
public:
    server_stopper() = default;                                          // not tied to any server: alive() is false
    HTTPP_API void stop() const noexcept;                                // as server::stop()
    HTTPP_API void shutdown(std::chrono::milliseconds grace = std::chrono::seconds(10)) const;  // as server::shutdown()
    HTTPP_API bool alive() const noexcept;                               // some `server` handle still holds the server

private:
    friend class server;
    std::weak_ptr<server::impl> impl_;
};

// A folder served as static files (ADR-0066): `directory(dir).set_index(false).mount(server, "/static")` (see server for what mounting a handle means). Honours Range, If-Modified-Since,
// ETag and HEAD, streams large files, and never leaves `dir` (".." segments and links out of it are refused). A setting is validated when
// it is set; mount() throws std::invalid_argument when the mount path does not start with '/'. The folder is read when requests arrive,
// so it may be created after mounting.
class directory {
public:
    HTTPP_API explicit directory(path root);
    // A path that ends in '/' (or the mount itself) is answered with index.html from that folder. Default: true.
    HTTPP_API directory& set_index(bool on);
    // false: a path with a segment that starts with '.' is a 404, whatever is on disk. Default: true.
    HTTPP_API directory& set_show_hidden(bool on);
    // false: a file's modified time is not sent: no "Last-Modified" and no "ETag" (the ETag holds the time), so no 304 either, and an
    // "If-Range" cannot be checked, so a range request that carries one gets the whole file. Every answer says "Cache-Control: no-cache".
    // Use it to keep the times of the files private. Default: true.
    HTTPP_API directory& set_show_dates(bool on);
    // true: send "X-Content-Type-Options: nosniff", and a file that is not plainly harmless to show (everything but plain text, JSON,
    // common images, PDF, audio and video) or that is asked for with "?download=1" gets "Content-Disposition: attachment" with a
    // filename and filename*=UTF-8''... Default: false.
    HTTPP_API directory& set_download_active(bool on);
    // true: html and svg files are shown inline instead of downloaded, but only under "Content-Security-Policy: sandbox; default-src 'none'; ...",
    // so no script runs and nothing is fetched from another origin (images, styles, fonts and media of the same server still load). Every other
    // active type (xml, js, unknown) stays a download, and "?download=1" still forces one. Only has an effect together with
    // set_download_active(true). Default: false.
    HTTPP_API directory& set_inline_active(bool on);
    // `s` is a handle (ADR-0081): the folder is mounted on the server it points to, so the caller's own `server` sees it. Returns that
    // handle, so the chain goes on: `auto srv = directory(p).mount(server(), "/").set_tls(cert, key);`
    HTTPP_API server mount(server s, const std::string& base = "/") const;

private:
    path root_;
    bool index_ = true;
    bool show_hidden_ = true;
    bool download_active_ = false;
    bool inline_active_ = false;
    bool show_dates_ = true;
};

// The Content-Type httpp gives a file by its extension (case-insensitive, the last dot after the last slash). Mounted directories use
// it, and so does Python's file(), so both agree. "application/octet-stream" when the extension is unknown or missing. See ADR-0047.
HTTPP_API const char* mime_type_for(const std::string& path);

} // namespace httpp
