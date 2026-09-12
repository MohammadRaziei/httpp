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

namespace httpp {

struct server::impl {
    httplib::Server svr;
};

server::server() : impl_(std::make_unique<impl>()) {}
server::~server() = default;
server::server(server&&) noexcept = default;
server& server::operator=(server&&) noexcept = default;

void server::get(const std::string& path, handler h) {
    impl_->svr.Get(path, [h](const httplib::Request& hreq, httplib::Response& hres) {
        request req;
        req.method = hreq.method;
        req.path = hreq.path;
        req.body = hreq.body;

        response res;
        res.status = 200;
        h(req, res);

        hres.status = res.status;
        hres.set_content(res.body, "text/plain");
    });
}

void server::serve_directory(const std::string& mount_path, const std::string& local_dir) {
    impl_->svr.set_mount_point(mount_path, local_dir);
}

int server::bind_to_any_port(const std::string& host) {
    return impl_->svr.bind_to_any_port(host);
}

void server::listen_after_bind() {
    impl_->svr.listen_after_bind();
}

void server::listen(const std::string& host, int port) {
    impl_->svr.listen(host, port);
}

void server::stop() {
    impl_->svr.stop();
}

} // namespace httpp
