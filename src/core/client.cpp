#include "httpp/client.hpp"

// httplib.h is included ONLY in this translation unit. It is compiled into
// httpp's .so/.a object code and is never included by any public httpp/*.hpp
// header, so consumers linking against httpp::core never see it and don't
// need it on their include path.
#include <httplib.h>

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

} // namespace httpp
