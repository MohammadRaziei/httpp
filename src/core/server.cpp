#include "httpp/server.hpp"

// httplib.h is included ONLY in this translation unit — never in a public
// httpp/*.hpp header — and stays hidden inside the compiled core.
#ifdef _WIN32
// Must come before <httplib.h> (which pulls in <winsock2.h>): without this,
// <windows.h> drags in the legacy <winsock.h> first and the two conflict.
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#endif
#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace httpp {

struct server::impl {
    httplib::Server svr;
};

server::server() : impl_(std::make_unique<impl>()) {
    // httplib's default is SO_REUSEPORT, which lets a second server bind a port
    // that is already being served, silently splitting the traffic. SO_REUSEADDR
    // alone keeps fast restarts (TIME_WAIT) but makes the second bind fail.
    // Windows: SO_REUSEADDR would allow exactly that hijack, so set nothing there.
    impl_->svr.set_socket_options([](socket_t sock) {
#ifndef _WIN32
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#else
        (void)sock;
#endif
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

} // namespace

void server::route(const std::string& method, const std::string& path, handler h) {
    auto fn = [h](const httplib::Request& hreq, httplib::Response& hres) {
        request req;
        req.method = hreq.method;
        req.path = hreq.path;
        req.body = hreq.body;
        req.headers = to_pairs(hreq.headers);
        req.query = to_pairs(hreq.params);
        req.path_params = to_pairs(hreq.path_params);

        response res;
        res.status = 200;
        h(req, res);

        hres.status = res.status;
        std::string content_type = "text/plain";
        for (const auto& [name, value] : res.headers) {
            if (ci_equal(name, "Content-Type")) content_type = value;
            else hres.set_header(name, value);
        }
        hres.set_content(res.body, content_type);
    };

    std::string m = method;
    std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return std::toupper(c); });
    auto& svr = impl_->svr;
    if (m == "GET") svr.Get(path, fn);
    else if (m == "POST") svr.Post(path, fn);
    else if (m == "PUT") svr.Put(path, fn);
    else if (m == "PATCH") svr.Patch(path, fn);
    else if (m == "DELETE") svr.Delete(path, fn);
    else if (m == "OPTIONS") svr.Options(path, fn);
    else throw std::invalid_argument("httpp::server::route: unsupported method '" + method + "'");
}

void server::get(const std::string& path, handler h) { route("GET", path, std::move(h)); }
void server::post(const std::string& path, handler h) { route("POST", path, std::move(h)); }
void server::put(const std::string& path, handler h) { route("PUT", path, std::move(h)); }
void server::patch(const std::string& path, handler h) { route("PATCH", path, std::move(h)); }
void server::del(const std::string& path, handler h) { route("DELETE", path, std::move(h)); }

void server::serve_directory(const std::string& mount_path, const std::string& local_dir) {
    impl_->svr.set_mount_point(mount_path, local_dir);
}

int server::bind_to_any_port(const std::string& host) {
    return impl_->svr.bind_to_any_port(host);
}

bool server::listen_after_bind() {
    return impl_->svr.listen_after_bind();
}

bool server::listen(const std::string& host, int port) {
    return impl_->svr.listen(host, port);
}

void server::stop() {
    impl_->svr.stop();
}

} // namespace httpp
