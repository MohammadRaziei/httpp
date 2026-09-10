#include "httpp/client.hpp"

// httplib.h is included ONLY in this translation unit. It is compiled into
// httpp's .so/.a object code and is never included by any public httpp/*.hpp
// header, so consumers linking against httpp::httpp never see it and don't
// need it on their include path.
#include <httplib.h>

namespace httpp {

struct Client::Impl {
    httplib::Client cli;
    Impl(const std::string& host, int port) : cli(host, port) {}
};

Client::Client(const std::string& host, int port)
    : impl_(std::make_unique<Impl>(host, port)) {}

Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

Response Client::get(const std::string& path) {
    Response out;
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
