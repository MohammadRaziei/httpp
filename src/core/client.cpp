#include "httpp/client.hpp"
#include "httpp/url.hpp"

// httplib.h is included ONLY in this translation unit. It is compiled into
// httpp's .so/.a object code and is never included by any public httpp/*.hpp
// header, so consumers linking against httpp::core never see it and don't
// need it on their include path.
#ifdef _WIN32
// Must come before <httplib.h> (which pulls in <winsock2.h>): without this,
// <windows.h> drags in the legacy <winsock.h> first and the two conflict.
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#endif
#include <httplib.h>

#include <stdexcept>

namespace httpp {

struct client::impl {
    httplib::Client cli;
    impl(const std::string& host, int port) : cli(host, port) {}
};

client::client(const std::string& host, int port)
    : impl_(std::make_unique<impl>(host, port)) {}

client::~client() = default;
client::client(client&&) noexcept = default;
client& client::operator=(client&&) noexcept = default;

response client::get(const std::string& path) {
    response out;
    auto res = impl_->cli.Get(path);
    if (res) {
        out.status = res->status;
        out.body = res->body;
    } else {
        out.status = 0; // connection/transport error
    }
    return out;
}

response client::fetch(const std::string& full_url) {
    const url u = url::parse(full_url);
    if (!u.valid()) {
        throw std::invalid_argument("httpp::client::fetch: unsupported or invalid URL: " + full_url);
    }
    client cli(u.host(), u.port());
    std::string path = u.path();
    if (!u.query().empty()) {
        path += "?" + u.query();
    }
    return cli.get(path);
}

} // namespace httpp
