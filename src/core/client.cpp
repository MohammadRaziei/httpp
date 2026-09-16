#include "httpp/client.hpp"
#include "httpp/url.hpp"

// httplib.h (+ mbedtls support) is included ONLY via this internal header,
// never in a public httpp/*.hpp header — see its comment for why.
#include "internal/httplib_common.hpp"

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace httpp {

// --- client ---

struct client::impl {
    httplib::Client cli;
    impl(const std::string& host, int port, bool use_ssl)
        : cli(detail::scheme_host_port(use_ssl ? "https" : "http", host, port)) {
        // get()/fetch() buffer the whole body into response::body, so the
        // same reasoning as client::request::run() applies: don't let
        // cpp-httplib's default 100MB cap turn a large but perfectly valid
        // response into an opaque status-0 failure.
        cli.set_payload_max_length((std::numeric_limits<std::size_t>::max)());
    }
};

client::client(const std::string& host, int port, bool use_ssl)
    : impl_(std::make_unique<impl>(host, port, use_ssl)) {}

client::~client() = default;
client::client(client&&) noexcept = default;
client& client::operator=(client&&) noexcept = default;

response client::get(const std::string& path) {
    return get(path, {});
}

response client::get(const std::string& path, const std::vector<std::pair<std::string, std::string>>& headers) {
    response out;
    httplib::Headers hdrs;
    for (const auto& [name, value] : headers) {
        hdrs.emplace(name, value);
    }
    auto res = impl_->cli.Get(path, hdrs);
    if (res) {
        out.status = res->status;
        out.body = res->body;
        for (const auto& [key, value] : res->headers) {
            out.headers.emplace_back(key, value);
        }
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
    client cli(u.host(), u.port(), u.scheme() == "https");
    std::string path = u.path();
    if (!u.query().empty()) {
        path += "?" + u.query();
    }
    return cli.get(path);
}

// --- request (fluent builder; also what curl_compat.cpp is built on) ---

struct client::request::impl {
    std::string url;
    std::string method;       // empty until set explicitly or via data()
    std::string body;
    std::string content_type = "application/x-www-form-urlencoded";
    std::vector<std::pair<std::string, std::string>> headers;
    long timeout_seconds = 0;  // 0 = use httplib's default
    bool follow_redirects = false;
    std::size_t max_response_size = 0;  // 0 = no limit (see header)

    explicit impl(std::string u) : url(std::move(u)) {}
};

client::request::request(std::string url) : impl_(std::make_unique<impl>(std::move(url))) {}
client::request::~request() = default;
client::request::request(client::request&&) noexcept = default;
client::request& client::request::operator=(client::request&&) noexcept = default;

client::request& client::request::method(std::string m) {
    impl_->method = std::move(m);
    return *this;
}

client::request& client::request::header(std::string name, std::string value) {
    impl_->headers.emplace_back(std::move(name), std::move(value));
    return *this;
}

client::request& client::request::data(std::string body) {
    impl_->body = std::move(body);
    if (impl_->method.empty()) {
        impl_->method = "POST";
    }
    return *this;
}

client::request& client::request::content_type(std::string type) {
    impl_->content_type = std::move(type);
    return *this;
}

client::request& client::request::timeout(long seconds) {
    impl_->timeout_seconds = seconds;
    return *this;
}

client::request& client::request::follow_redirects(bool enable) {
    impl_->follow_redirects = enable;
    return *this;
}

client::request& client::request::max_response_size(std::size_t bytes) {
    impl_->max_response_size = bytes;
    return *this;
}

response client::request::run() const {
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

    const std::string method = impl_->method.empty() ? "GET" : impl_->method;
    httplib::Client cli(detail::scheme_host_port(u.scheme(), u.host(), u.port()));
    if (impl_->timeout_seconds > 0) {
        cli.set_connection_timeout(impl_->timeout_seconds);
        cli.set_read_timeout(impl_->timeout_seconds);
    }
    cli.set_follow_location(impl_->follow_redirects);

    // cpp-httplib caps a buffered response body at CPPHTTPLIB_PAYLOAD_MAX_LENGTH
    // (100MB) by default, and exceeding it surfaces as a transport-style
    // failure that is indistinguishable from a dropped connection. httpp's
    // contract here is "give me the whole body as a string", so the limit is
    // opt-in via max_response_size() instead — see the header. Note the cap
    // only applies to this buffered path; httpp::download(...).run() streams
    // and is unaffected either way.
    cli.set_payload_max_length(impl_->max_response_size > 0
                                   ? impl_->max_response_size
                                   : (std::numeric_limits<std::size_t>::max)());

    httplib::Result res;
    if (method == "GET") {
        res = cli.Get(path, hdrs);
    } else if (method == "HEAD") {
        // A HEAD response is headers-only by definition; res->body stays
        // empty, which is correct rather than a failure.
        res = cli.Head(path, hdrs);
    } else if (method == "OPTIONS") {
        res = cli.Options(path, hdrs);
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
        for (const auto& [key, value] : res->headers) {
            out.headers.emplace_back(key, value);
        }
    } else if (res.error() == httplib::Error::ExceedMaxPayloadSize) {
        // Caller-imposed max_response_size was hit. Report it as 413 so it
        // is distinguishable from a genuine connection failure (status 0).
        out.status = 413;
    } else {
        out.status = 0; // connection/transport error
    }
    return out;
}

} // namespace httpp
