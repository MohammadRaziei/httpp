#include "httpp/curl.hpp"
#include "httpp/client.hpp"

#include <utility>

namespace httpp::curl {

// A curl-flavoured face on httpp::client::request: same engine, same policy, nothing of its own.
struct request::impl {
    client::request req;
    explicit impl(std::string url) : req(std::move(url)) {}
};

request::request(std::string url) : impl_(std::make_unique<impl>(std::move(url))) {}
request::~request() = default;
request::request(request&&) noexcept = default;
request& request::operator=(request&&) noexcept = default;

request& request::method(std::string m) {
    impl_->req.method(std::move(m));
    return *this;
}

request& request::header(std::string name, std::string value) {
    impl_->req.header(std::move(name), std::move(value));
    return *this;
}

request& request::data(std::string body) {
    impl_->req.data(std::move(body)); // POST unless a method was set, like curl -d
    return *this;
}

request& request::content_type(std::string type) {
    impl_->req.content_type(std::move(type));
    return *this;
}

request& request::timeout(long seconds) {
    impl_->req.timeout(seconds);
    return *this;
}

request& request::follow_redirects(bool enable) {
    impl_->req.follow_redirects(enable);
    return *this;
}

response request::run() const { return impl_->req.run(); } // status 0 on a bad URL, method or transport error

} // namespace httpp::curl
