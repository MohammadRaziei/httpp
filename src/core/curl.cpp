#include "httpp/curl.hpp"
#include "httpp/url.hpp"

// httplib.h is included ONLY in this translation unit — never in a public
// httpp/*.hpp header — and stays hidden inside the compiled core.
#include <httplib.h>

#include <utility>
#include <vector>

namespace httpp::curl {

struct request::impl {
    std::string url;
    std::string method;       // empty until set explicitly or via data()
    std::string body;
    std::string content_type = "application/x-www-form-urlencoded";
    std::vector<std::pair<std::string, std::string>> headers;
    long timeout_seconds = 0;      // 0 = use httplib's default
    bool follow_redirects = false;

    explicit impl(std::string u) : url(std::move(u)) {}
};

request::request(std::string url) : impl_(std::make_unique<impl>(std::move(url))) {}
request::~request() = default;
request::request(request&&) noexcept = default;
request& request::operator=(request&&) noexcept = default;

request& request::method(std::string m) {
    impl_->method = std::move(m);
    return *this;
}

request& request::header(std::string name, std::string value) {
    impl_->headers.emplace_back(std::move(name), std::move(value));
    return *this;
}

request& request::data(std::string body) {
    impl_->body = std::move(body);
    if (impl_->method.empty()) {
        impl_->method = "POST";
    }
    return *this;
}

request& request::content_type(std::string type) {
    impl_->content_type = std::move(type);
    return *this;
}

request& request::timeout(long seconds) {
    impl_->timeout_seconds = seconds;
    return *this;
}

request& request::follow_redirects(bool enable) {
    impl_->follow_redirects = enable;
    return *this;
}

response request::run() const {
    response out;

    const url u = url::parse(impl_->url);
    if (!u.valid()) {
        out.status = 0;
        return out;
    }

    std::string path = u.path();
    if (!u.query().empty()) {
        path += "?" + u.query();
    }

    httplib::Headers hdrs;
    for (const auto& [name, value] : impl_->headers) {
        hdrs.emplace(name, value);
    }

    std::string method = impl_->method.empty() ? "GET" : impl_->method;
    httplib::Client cli(u.host(), u.port());
    if (impl_->timeout_seconds > 0) {
        cli.set_connection_timeout(impl_->timeout_seconds);
        cli.set_read_timeout(impl_->timeout_seconds);
    }
    cli.set_follow_location(impl_->follow_redirects);

    httplib::Result res;
    if (method == "GET") {
        res = cli.Get(path, hdrs);
    } else if (method == "POST") {
        res = cli.Post(path, hdrs, impl_->body, impl_->content_type);
    } else if (method == "PUT") {
        res = cli.Put(path, hdrs, impl_->body, impl_->content_type);
    } else if (method == "PATCH") {
        res = cli.Patch(path, hdrs, impl_->body, impl_->content_type);
    } else if (method == "DELETE") {
        res = cli.Delete(path, hdrs, impl_->body, impl_->content_type);
    } else {
        out.status = 0; // unsupported method
        return out;
    }

    if (res) {
        out.status = res->status;
        out.body = res->body;
    } else {
        out.status = 0; // connection/transport error
    }
    return out;
}

} // namespace httpp::curl
