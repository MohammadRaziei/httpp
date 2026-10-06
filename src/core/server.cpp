#include "httpp/server.hpp"

// httplib.h is included ONLY in this translation unit — never in a public
// httpp/*.hpp header — and stays hidden inside the compiled core.
#ifdef _WIN32
// Must come before <httplib.h> (which pulls in <winsock2.h>): without this,
// <windows.h> drags in the legacy <winsock.h> first and the two conflict.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif
#include <httplib.h>

#include "internal/event_loop.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace httpp {

struct server::impl {
    httplib::Server svr;
    // Accepts connections and watches idle ones (Asio). Worker threads only ever
    // see a connection while one of its requests is being read and answered.
    std::unique_ptr<detail::event_loop> loop;
    std::atomic<httplib::TaskQueue*> pool{nullptr}; // alive only while listening
    std::atomic<int> async_timeout{60};
    server::exception_hook exception_hook; // also used by route_async handlers

    // Runs on a worker thread: serve one request, then hand the connection back
    // to the loop (keep-alive) or close it.
    void serve(socket_t sock, std::size_t remaining) {
        const auto result = svr.serve_one(sock, remaining);
        if (result.detached) return; // a deferred response owns the socket now
        if (result.keep && loop->give_back(static_cast<detail::native_socket>(sock), remaining - 1)) return;
        httplib::detail::drain_and_close_socket(sock);
    }
};

server::server() : impl_(std::make_unique<impl>()) {
    impl* im = impl_.get();
    detail::event_loop::options opt;
    opt.keep_alive_timeout_sec = CPPHTTPLIB_KEEPALIVE_TIMEOUT_SECOND;
    opt.keep_alive_max = CPPHTTPLIB_KEEPALIVE_MAX_COUNT;
    im->loop = std::make_unique<detail::event_loop>(
        opt,
        [im](detail::native_socket fd, std::size_t remaining) {
            const auto sock = static_cast<socket_t>(fd);
            auto* queue = im->pool.load();
            if (!queue || !queue->enqueue([im, sock, remaining] { im->serve(sock, remaining); })) {
                httplib::detail::drain_and_close_socket(sock);
            }
        },
        [im] {
            if (auto* queue = im->pool.load()) queue->on_idle(); // let the pool shrink again
        });
    // A deferred (async) response may keep its connection alive too.
    im->svr.set_connection_releaser([im](socket_t sock, std::size_t remaining) {
        return im->loop->give_back(static_cast<detail::native_socket>(sock), remaining);
    });
}
server::~server() = default;
server::server(server&&) noexcept = default;
server& server::operator=(server&&) noexcept = default;

namespace {

template <class Map>
std::vector<std::pair<std::string, std::string>> to_pairs(const Map& m) {
    return {m.begin(), m.end()};
}

bool ci_equal(const std::string& a, const char* b) {
    return a.size() == std::strlen(b) &&
           std::equal(a.begin(), a.end(), b, [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

request to_request(const httplib::Request& hreq) {
    request req;
    req.method = hreq.method;
    req.path = hreq.path;
    req.body = hreq.body;
    req.headers = to_pairs(hreq.headers);
    req.query = to_pairs(hreq.params);
    req.path_params = to_pairs(hreq.path_params);
    return req;
}

void fill(httplib::Response& hres, const response& res) {
    hres.status = res.status > 0 ? res.status : 200;
    std::string content_type = "text/plain";
    for (const auto& [name, value] : res.headers) {
        if (ci_equal(name, "Content-Type")) content_type = value;
        else hres.set_header(name, value);
    }
    if (!res.file.empty()) {
        // Known length, read from disk as the response is written (so it also works for async
        // responses, which are written later from another thread).
        // ponytail: size via ifstream (no <filesystem>: it needs macOS >= 10.15 and the wheels target 10.13).
        auto in = std::make_shared<std::ifstream>(res.file, std::ios::binary | std::ios::ate);
        const std::streamoff size = *in ? static_cast<std::streamoff>(in->tellg()) : -1;
        if (size >= 0) in->seekg(0);
        // a directory opens fine but cannot be read: peek turns that into a 404
        if (size < 0 || (size > 0 && in->peek() == std::ifstream::traits_type::eof())) {
            hres.status = 404;
            hres.set_content("Not Found", "text/plain");
            return;
        }
        hres.set_content_provider(static_cast<std::size_t>(size), content_type,
                                  [in](std::size_t offset, std::size_t length, httplib::DataSink& sink) {
                                      std::string buf(std::min<std::size_t>(length, 64 * 1024), '\0');
                                      in->clear();
                                      in->seekg(static_cast<std::streamoff>(offset));
                                      in->read(&buf[0], static_cast<std::streamsize>(buf.size()));
                                      const auto got = static_cast<std::size_t>(in->gcount());
                                      return got > 0 && sink.write(buf.data(), got);
                                  });
    } else if (res.stream) {
        auto next = res.stream;
        hres.set_chunked_content_provider(content_type, [next](std::size_t offset, httplib::DataSink& sink) {
            std::string chunk;
            bool more = false;
            try {
                more = next(offset, chunk);
            } catch (...) {
                return false; // abort the response; the connection is dropped
            }
            if (!chunk.empty() && !sink.write(chunk.data(), chunk.size())) return false;
            if (!more) sink.done();
            return true;
        });
    } else {
        hres.set_content(res.body, content_type);
    }
}

using http_fn = std::function<void(const httplib::Request&, httplib::Response&)>;

void register_route(httplib::Server& svr, const std::string& method, const std::string& path, http_fn fn) {
    std::string m = method;
    std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return std::toupper(c); });
    if (m == "GET") svr.Get(path, fn);
    else if (m == "POST") svr.Post(path, fn);
    else if (m == "PUT") svr.Put(path, fn);
    else if (m == "PATCH") svr.Patch(path, fn);
    else if (m == "DELETE") svr.Delete(path, fn);
    else if (m == "OPTIONS") svr.Options(path, fn);
    else throw std::invalid_argument("httpp::server::route: unsupported method '" + method + "'");
}

} // namespace

struct responder::impl {
    std::shared_ptr<httplib::DetachedResponse> detached;
};

void responder::send(response res) const {
    if (!impl_ || !impl_->detached) return;
    httplib::Response hres;
    fill(hres, res);
    impl_->detached->complete(std::move(hres));
}

void server::route(const std::string& method, const std::string& path, handler h) {
    register_route(impl_->svr, method, path, [h](const httplib::Request& hreq, httplib::Response& hres) {
        request req = to_request(hreq);
        response res;
        res.status = 200;
        h(req, res);
        fill(hres, res);
    });
}

void server::route_async(const std::string& method, const std::string& path, async_handler h) {
    impl* im = impl_.get();
    register_route(impl_->svr, method, path, [im, h](const httplib::Request& hreq, httplib::Response& hres) {
        request req = to_request(hreq);
        responder r;
        r.impl_ = std::make_shared<responder::impl>();
        r.impl_->detached = hres.detach(); // from here on, `hres` is no longer ours to fill in

        // If nobody answers in time, answer 504 ourselves. The timer holds only a weak reference,
        // so a response that was sent (and dropped) in the meantime costs nothing.
        const int timeout = im->async_timeout.load();
        if (timeout > 0) {
            std::weak_ptr<responder::impl> weak = r.impl_;
            im->loop->after(std::chrono::seconds(timeout), [im, weak] {
                auto strong = weak.lock();
                if (!strong) return;
                responder late;
                late.impl_ = strong;
                auto answer = [late] {
                    response err;
                    err.status = 504;
                    err.body = "Gateway Timeout";
                    late.send(err); // no-op if it was answered after all
                };
                // The write may block on a slow client: do it on a worker, not on the loop thread.
                if (auto* queue = im->pool.load(); !queue || !queue->enqueue(answer)) answer();
            });
        }

        try {
            h(req, r);
        } catch (...) {
            response err;
            err.status = 500;
            err.body = "Internal Server Error";
            if (im->exception_hook) {
                try { im->exception_hook(req, err, std::current_exception()); } catch (...) {}
            }
            r.send(err); // no-op if the handler had already answered
        }
    });
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

server& server::before(before_hook hook) {
    impl_->svr.set_pre_routing_handler(
        [hook](const httplib::Request& hreq, httplib::Response& hres) {
            response out;
            out.status = 0;
            if (!hook(to_request(hreq), out)) return httplib::Server::HandlerResponse::Unhandled;
            fill(hres, out);
            return httplib::Server::HandlerResponse::Handled;
        });
    return *this;
}

server& server::after(after_hook hook) {
    impl_->svr.set_post_routing_handler([hook](const httplib::Request& hreq, httplib::Response& hres) {
        response out; // status and headers only: the body is not copied (it may be huge or streamed)
        out.status = hres.status;
        out.headers = to_pairs(hres.headers);
        hook(to_request(hreq), out);
        hres.status = out.status;
        hres.headers.clear();
        for (const auto& [name, value] : out.headers) hres.headers.emplace(name, value);
    });
    return *this;
}

server& server::set_error_handler(error_hook hook) {
    httplib::Server::Handler handler = [hook](const httplib::Request& hreq, httplib::Response& hres) {
        // cpp-httplib calls this for EVERY 4xx/5xx, including ones a route handler produced on
        // purpose (e.g. {"error": "not found"}, 404). Only errors the server made itself, which have
        // no body yet, are ours to dress up.
        if (!hres.body.empty()) return;
        response out;
        out.status = hres.status;
        hook(to_request(hreq), out);
        fill(hres, out);
    };
    impl_->svr.set_error_handler(std::move(handler));
    return *this;
}

server& server::set_exception_handler(exception_hook hook) {
    impl_->exception_hook = hook;
    impl_->svr.set_exception_handler(
        [hook](const httplib::Request& hreq, httplib::Response& hres, std::exception_ptr ep) {
            response out;
            out.status = 500;
            out.body = "Internal Server Error";
            hook(to_request(hreq), out, ep);
            fill(hres, out);
        });
    return *this;
}

server& server::set_thread_pool(std::size_t base, std::size_t max) {
    if (base == 0) base = 1;
    if (max < base) max = base * 4;
    impl_->svr.new_task_queue = [base, max] { return new httplib::ThreadPool(base, max); };
    return *this;
}
server& server::set_read_timeout(int seconds) { impl_->svr.set_read_timeout(seconds); return *this; }
server& server::set_write_timeout(int seconds) { impl_->svr.set_write_timeout(seconds); return *this; }
server& server::set_keep_alive_timeout(int seconds) {
    impl_->svr.set_keep_alive_timeout(seconds);
    impl_->loop->set_keep_alive_timeout(seconds);
    return *this;
}
server& server::set_keep_alive_max(std::size_t requests) {
    impl_->svr.set_keep_alive_max_count(requests == 0 ? 1 : requests);
    impl_->loop->set_keep_alive_max(requests);
    return *this;
}
server& server::set_max_body(std::size_t bytes) {
    impl_->svr.set_payload_max_length(bytes == 0 ? (std::numeric_limits<std::size_t>::max)() : bytes);
    return *this;
}
server& server::set_async_timeout(int seconds) {
    impl_->async_timeout = seconds < 0 ? 0 : seconds;
    return *this;
}

void server::serve_directory(const std::string& mount_path, const std::string& local_dir) {
    impl_->svr.set_mount_point(mount_path, local_dir);
}

int server::bind_to_any_port(const std::string& host) {
    return impl_->loop->bind(host, 0);
}

bool server::listen_after_bind() {
    std::unique_ptr<httplib::TaskQueue> queue(impl_->svr.new_task_queue());
    impl_->pool = queue.get();
    impl_->svr.set_external_loop(true); // else httplib thinks it is shutting down and cuts off streamed bodies
    const bool ok = impl_->loop->run(); // returns after stop()
    impl_->svr.set_external_loop(false);
    queue->shutdown();                  // joins the workers; they may still hand sockets back (refused now)
    impl_->pool = nullptr;
    return ok;
}

bool server::listen(const std::string& host, int port) {
    if (impl_->loop->bind(host, port) < 0) return false;
    return listen_after_bind();
}

void server::stop() {
    impl_->svr.set_external_loop(false); // in-flight streamed bodies stop promptly
    impl_->loop->stop();
}

} // namespace httpp
