#include "httpp/client.hpp"
#include "httpp/url.hpp"

// httplib.h (+ mbedtls support) is included ONLY via this internal header,
// never in a public httpp/*.hpp header — see its comment for why.
#include "internal/httplib_common.hpp"
#include "internal/blocking_pool.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace httpp {
namespace {

constexpr std::size_t kNoLimit = (std::numeric_limits<std::size_t>::max)();

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

error_kind classify(httplib::Error e) {
    using E = httplib::Error;
    switch (e) {
        case E::ConnectionTimeout:
        case E::Timeout:
            return error_kind::timeout;
        case E::Connection:
        case E::ConnectionClosed:
        case E::ProxyConnection:
        case E::BindIPAddress:
        case E::Read:
        case E::Write:
            return error_kind::connection;
        case E::SSLConnection:
        case E::SSLLoadingCerts:
        case E::SSLServerVerification:
        case E::SSLServerHostnameVerification:
            return error_kind::tls;
        case E::ExceedRedirectCount:
            return error_kind::too_many_redirects;
        case E::ExceedMaxPayloadSize:
            return error_kind::too_large;
        case E::Canceled:
            return error_kind::canceled;
        default:
            return error_kind::other;
    }
}

void fail(response& out, error_kind kind, std::string message) {
    out.status = 0;
    out.error = kind;
    out.error_message = std::move(message);
}

using clock_type = std::chrono::steady_clock;

// Turn a cpp-httplib result into an httpp::response, keeping the reason when there is no HTTP response.
// cpp-httplib reports a read that timed out as a plain Error::Read, indistinguishable from a dropped
// connection, so when a timeout was configured and the call ran for (about) that long, call it a timeout.
void fill(response& out, httplib::Result& res, clock_type::time_point started, long timeout_seconds) {
    if (res) {
        out.status = res->status;
        out.body = res->body;
        for (const auto& [key, value] : res->headers) out.headers.emplace_back(key, value);
        return;
    }
    auto kind = classify(res.error());
    if (kind == error_kind::connection && timeout_seconds > 0 &&
        (res.error() == httplib::Error::Read || res.error() == httplib::Error::Write)) {
        const double elapsed = std::chrono::duration<double>(clock_type::now() - started).count();
        if (elapsed >= static_cast<double>(timeout_seconds) - 0.25) kind = error_kind::timeout;
    }
    fail(out, kind, kind == error_kind::timeout ? "timed out" : httplib::to_string(res.error()));
    if (kind == error_kind::too_large) {
        // Caller-imposed max_response_size was hit: 413 keeps it apart from a dropped connection.
        out.status = 413;
        out.error_message = "response body exceeded max_response_size";
    }
}

void apply_tls(httplib::Client& cli, bool verify, const std::string& ca_file) {
    if (!ca_file.empty()) cli.set_ca_cert_path(ca_file);
    if (!verify) {
        cli.enable_server_certificate_verification(false);
        cli.enable_server_hostname_verification(false);
    }
}

// Returns false for a method httpp does not support.
bool dispatch(httplib::Client& cli, const std::string& method, const std::string& path,
              const httplib::Headers& hdrs, const std::string& body, const std::string& content_type,
              httplib::Result& res) {
    const std::string ctype = content_type.empty() ? "application/x-www-form-urlencoded" : content_type;
    if (method == "GET") res = cli.Get(path, hdrs);
    else if (method == "HEAD") res = cli.Head(path, hdrs); // headers-only by definition: an empty body is correct
    else if (method == "OPTIONS") res = cli.Options(path, hdrs);
    else if (method == "POST") res = cli.Post(path, hdrs, body, ctype);
    else if (method == "PUT") res = cli.Put(path, hdrs, body, ctype);
    else if (method == "PATCH") res = cli.Patch(path, hdrs, body, ctype);
    else if (method == "DELETE") res = cli.Delete(path, hdrs, body, ctype);
    else return false;
    return true;
}

std::string join_path(const std::string& base, const std::string& path) {
    if (path.empty()) return base.empty() ? "/" : base;
    return base + (path[0] == '/' ? "" : "/") + path;
}

} // namespace

// --- client (a keep-alive session) ---

struct client::impl {
    std::mutex mu;
    httplib::Client cli;
    std::string base_path;
    httplib::Headers defaults;
    long timeout_seconds = 0; // what set_timeout() asked for (0 = httplib's defaults)

    explicit impl(const std::string& origin) : cli(origin) {
        // get()/fetch() buffer the whole body into response::body, so don't let cpp-httplib's
        // default 100MB cap turn a large but valid response into an opaque failure.
        cli.set_payload_max_length(kNoLimit);
        cli.set_keep_alive(true);
    }
};

namespace {
std::string origin_of(const url& u) { return detail::scheme_host_port(u.scheme(), u.host(), u.port()); }

} // namespace

client::client(const std::string& host, int port, bool use_ssl)
    : impl_(std::make_unique<impl>(detail::scheme_host_port(use_ssl ? "https" : "http", host, port))) {}

client::client(const std::string& base_url) {
    const url u = url::parse(base_url);
    if (!u.valid()) throw std::invalid_argument("httpp::client: not a usable http(s) URL: " + base_url);
    impl_ = std::make_unique<impl>(origin_of(u));
    std::string p = u.path();
    while (!p.empty() && p.back() == '/') p.pop_back();
    impl_->base_path = p;
}

client::~client() = default;
client::client(client&&) noexcept = default;
client& client::operator=(client&&) noexcept = default;

response client::send(const std::string& method, const std::string& path, const headers_t& headers,
                      const std::string& body, const std::string& content_type) {
    response out;
    std::lock_guard<std::mutex> lock(impl_->mu);
    httplib::Headers hdrs = impl_->defaults;
    for (const auto& [name, value] : headers) {
        hdrs.erase(name); // a per-call header replaces a session default of the same name
        hdrs.emplace(name, value);
    }
    httplib::Result res;
    const auto started = clock_type::now();
    if (!dispatch(impl_->cli, upper(method), join_path(impl_->base_path, path), hdrs, body, content_type, res)) {
        fail(out, error_kind::unsupported_method, "unsupported HTTP method: " + method);
        return out;
    }
    fill(out, res, started, impl_->timeout_seconds);
    return out;
}

response client::get(const std::string& path) { return send("GET", path); }
response client::get(const std::string& path, const headers_t& headers) { return send("GET", path, headers); }
response client::head(const std::string& path, const headers_t& headers) { return send("HEAD", path, headers); }
response client::options(const std::string& path, const headers_t& headers) { return send("OPTIONS", path, headers); }
response client::post(const std::string& path, const std::string& body, const std::string& ct, const headers_t& h) {
    return send("POST", path, h, body, ct);
}
response client::put(const std::string& path, const std::string& body, const std::string& ct, const headers_t& h) {
    return send("PUT", path, h, body, ct);
}
response client::patch(const std::string& path, const std::string& body, const std::string& ct, const headers_t& h) {
    return send("PATCH", path, h, body, ct);
}
response client::del(const std::string& path, const std::string& body, const std::string& ct, const headers_t& h) {
    return send("DELETE", path, h, body, ct);
}

client& client::set_header(std::string name, std::string value) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->defaults.erase(name);
    impl_->defaults.emplace(std::move(name), std::move(value));
    return *this;
}
client& client::set_timeout(long seconds) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (seconds > 0) {
        impl_->timeout_seconds = seconds;
        impl_->cli.set_connection_timeout(seconds);
        impl_->cli.set_read_timeout(seconds);
        impl_->cli.set_write_timeout(seconds);
    }
    return *this;
}
client& client::set_follow_redirects(bool enable) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->cli.set_follow_location(enable);
    return *this;
}
client& client::set_basic_auth(const std::string& user, const std::string& password) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->cli.set_basic_auth(user, password);
    return *this;
}
client& client::set_bearer_token(const std::string& token) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->cli.set_bearer_token_auth(token);
    return *this;
}
client& client::set_proxy(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->cli.set_proxy(host, port);
    return *this;
}
client& client::set_verify(bool enable) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    apply_tls(impl_->cli, enable, "");
    if (enable) { // turning verification back on
        impl_->cli.enable_server_certificate_verification(true);
        impl_->cli.enable_server_hostname_verification(true);
    }
    return *this;
}
client& client::set_ca_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    apply_tls(impl_->cli, true, path);
    return *this;
}
client& client::set_keep_alive(bool enable) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->cli.set_keep_alive(enable);
    return *this;
}

response client::fetch(const std::string& full_url) {
    const url u = url::parse(full_url);
    if (!u.valid()) {
        throw std::invalid_argument("httpp::client::fetch: unsupported or invalid URL: " + full_url);
    }
    client cli(u.host(), u.port(), u.scheme() == "https");
    cli.set_keep_alive(false);
    std::string path = u.path();
    if (!u.query().empty()) path += "?" + u.query();
    return cli.get(path);
}

// --- request (fluent builder; also what curl_compat.cpp is built on) ---

struct client::request::impl {
    std::string url;
    std::string method;       // empty until set explicitly or via a body
    std::string body;
    std::string content_type = "application/x-www-form-urlencoded";
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::pair<std::string, std::string>> params;
    std::vector<std::pair<std::string, std::string>> cookies;
    std::string auth_user, auth_password, bearer;
    bool has_basic_auth = false;
    long timeout_seconds = 0;  // 0 = use httplib's default
    bool follow_redirects = false;
    bool verify = true;
    std::string ca_file;
    std::string proxy_host;
    int proxy_port = 0;
    std::size_t max_response_size = 0;  // 0 = no limit (see header)

    explicit impl(std::string u) : url(std::move(u)) {}
};

response client::request::run_impl(const impl& r) {
    response out;

    const url u = url::parse(r.url);
    if (!u.valid()) {
        fail(out, error_kind::invalid_url, "not a usable http(s) URL: " + r.url);
        return out;
    }

    std::string path = u.path();
    std::string query = u.query();
    if (!r.params.empty()) {
        if (!query.empty()) query += "&";
        query += url::build_query(r.params);
    }
    if (!query.empty()) path += "?" + query;

    httplib::Headers hdrs;
    for (const auto& [name, value] : r.headers) hdrs.emplace(name, value);
    if (!r.cookies.empty() && hdrs.find("Cookie") == hdrs.end()) {
        std::string joined;
        for (const auto& [name, value] : r.cookies) {
            if (!joined.empty()) joined += "; ";
            joined += name + "=" + value;
        }
        hdrs.emplace("Cookie", joined);
    }

    httplib::Client cli(origin_of(u));
    if (r.timeout_seconds > 0) {
        cli.set_connection_timeout(r.timeout_seconds);
        cli.set_read_timeout(r.timeout_seconds);
        cli.set_write_timeout(r.timeout_seconds);
    }
    cli.set_follow_location(r.follow_redirects);
    apply_tls(cli, r.verify, r.ca_file);
    if (r.has_basic_auth) cli.set_basic_auth(r.auth_user, r.auth_password);
    if (!r.bearer.empty()) cli.set_bearer_token_auth(r.bearer);
    if (!r.proxy_host.empty()) cli.set_proxy(r.proxy_host, r.proxy_port);

    // cpp-httplib caps a buffered response body at CPPHTTPLIB_PAYLOAD_MAX_LENGTH (100MB) by
    // default and exceeding it looks like a dropped connection. httpp's contract here is "give
    // me the whole body as a string", so the limit is opt-in via max_response_size() instead.
    // (The cap only applies to this buffered path; httpp::download(...).run() streams.)
    cli.set_payload_max_length(r.max_response_size > 0 ? r.max_response_size : kNoLimit);

    httplib::Result res;
    const auto started = clock_type::now();
    if (!dispatch(cli, upper(r.method.empty() ? "GET" : r.method), path, hdrs, r.body, r.content_type, res)) {
        fail(out, error_kind::unsupported_method, "unsupported HTTP method: " + r.method);
        return out;
    }
    fill(out, res, started, r.timeout_seconds);
    return out;
}

client::request::request(std::string url) : impl_(std::make_unique<impl>(std::move(url))) {}
client::request::~request() = default;
client::request::request(client::request&&) noexcept = default;
client::request& client::request::operator=(client::request&&) noexcept = default;

client::request& client::request::method(std::string m) { impl_->method = std::move(m); return *this; }
client::request& client::request::header(std::string name, std::string value) {
    impl_->headers.emplace_back(std::move(name), std::move(value));
    return *this;
}
client::request& client::request::param(std::string name, std::string value) {
    impl_->params.emplace_back(std::move(name), std::move(value));
    return *this;
}
client::request& client::request::data(std::string body) {
    impl_->body = std::move(body);
    if (impl_->method.empty()) impl_->method = "POST";
    return *this;
}
client::request& client::request::json(std::string document) {
    impl_->body = std::move(document);
    impl_->content_type = "application/json";
    if (impl_->method.empty()) impl_->method = "POST";
    return *this;
}
client::request& client::request::form(const std::vector<std::pair<std::string, std::string>>& fields) {
    impl_->body = url::build_query(fields);
    impl_->content_type = "application/x-www-form-urlencoded";
    if (impl_->method.empty()) impl_->method = "POST";
    return *this;
}
client::request& client::request::content_type(std::string type) { impl_->content_type = std::move(type); return *this; }
client::request& client::request::basic_auth(const std::string& user, const std::string& password) {
    impl_->has_basic_auth = true;
    impl_->auth_user = user;
    impl_->auth_password = password;
    return *this;
}
client::request& client::request::bearer(const std::string& token) { impl_->bearer = token; return *this; }
client::request& client::request::cookie(std::string name, std::string value) {
    impl_->cookies.emplace_back(std::move(name), std::move(value));
    return *this;
}
client::request& client::request::timeout(long seconds) { impl_->timeout_seconds = seconds; return *this; }
client::request& client::request::follow_redirects(bool enable) { impl_->follow_redirects = enable; return *this; }
client::request& client::request::verify(bool enable) { impl_->verify = enable; return *this; }
client::request& client::request::ca_file(std::string path) { impl_->ca_file = std::move(path); return *this; }
client::request& client::request::proxy(std::string host, int port) {
    impl_->proxy_host = std::move(host);
    impl_->proxy_port = port;
    return *this;
}
client::request& client::request::max_response_size(std::size_t bytes) { impl_->max_response_size = bytes; return *this; }

response client::request::run() const { return run_impl(*impl_); }

std::future<response> client::request::run_async() const {
    auto promise = std::make_shared<std::promise<response>>();
    auto future = promise->get_future();
    auto snapshot = std::make_shared<impl>(*impl_); // the builder may be destroyed before the job runs
    detail::blocking_pool::instance().submit([promise, snapshot] {
        try {
            promise->set_value(run_impl(*snapshot));
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    return future;
}

// --- async_client (a pool of keep-alive connections) ---

struct async_client::impl {
    std::string base_url;
    std::size_t max_connections;

    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::unique_ptr<client>> free;
    std::size_t created = 0;

    // settings applied to every connection created
    std::vector<std::pair<std::string, std::string>> headers;
    long timeout = 0;
    bool follow = false;
    bool has_basic = false;
    std::string user, password, bearer, ca_file;
    bool verify = true;

    std::unique_ptr<client> make() const {
        auto c = std::make_unique<client>(base_url);
        for (const auto& [name, value] : headers) c->set_header(name, value);
        if (timeout > 0) c->set_timeout(timeout);
        c->set_follow_redirects(follow);
        if (has_basic) c->set_basic_auth(user, password);
        if (!bearer.empty()) c->set_bearer_token(bearer);
        if (!ca_file.empty()) c->set_ca_file(ca_file);
        if (!verify) c->set_verify(false);
        return c;
    }

    std::unique_ptr<client> acquire() {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [this] { return !free.empty() || created < max_connections; });
        if (!free.empty()) {
            auto c = std::move(free.back());
            free.pop_back();
            return c;
        }
        created++;
        lock.unlock();
        return make();
    }

    void release(std::unique_ptr<client> c) {
        {
            std::lock_guard<std::mutex> lock(mu);
            free.push_back(std::move(c));
        }
        cv.notify_one();
    }
};

async_client::async_client(const std::string& base_url, std::size_t max_connections)
    : impl_(std::make_shared<impl>()) {
    impl_->base_url = base_url;
    impl_->max_connections = max_connections == 0 ? 1 : max_connections;
    client probe(base_url); // validates the URL now (throws std::invalid_argument), not on the first request
}
async_client::~async_client() = default;
async_client::async_client(async_client&&) noexcept = default;
async_client& async_client::operator=(async_client&&) noexcept = default;

std::future<response> async_client::send(const std::string& method, const std::string& path,
                                         const headers_t& headers, const std::string& body,
                                         const std::string& content_type) {
    auto promise = std::make_shared<std::promise<response>>();
    auto future = promise->get_future();
    auto state = impl_;
    detail::blocking_pool::instance().submit([=] {
        try {
            auto c = state->acquire();
            response r = c->send(method, path, headers, body, content_type);
            state->release(std::move(c));
            promise->set_value(std::move(r));
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    return future;
}

std::future<response> async_client::get(const std::string& path, const headers_t& h) { return send("GET", path, h); }
std::future<response> async_client::head(const std::string& path, const headers_t& h) { return send("HEAD", path, h); }
std::future<response> async_client::post(const std::string& p, const std::string& b, const std::string& ct, const headers_t& h) {
    return send("POST", p, h, b, ct);
}
std::future<response> async_client::put(const std::string& p, const std::string& b, const std::string& ct, const headers_t& h) {
    return send("PUT", p, h, b, ct);
}
std::future<response> async_client::patch(const std::string& p, const std::string& b, const std::string& ct, const headers_t& h) {
    return send("PATCH", p, h, b, ct);
}
std::future<response> async_client::del(const std::string& p, const std::string& b, const std::string& ct, const headers_t& h) {
    return send("DELETE", p, h, b, ct);
}

async_client& async_client::set_header(std::string name, std::string value) {
    impl_->headers.emplace_back(std::move(name), std::move(value));
    return *this;
}
async_client& async_client::set_timeout(long seconds) { impl_->timeout = seconds; return *this; }
async_client& async_client::set_follow_redirects(bool enable) { impl_->follow = enable; return *this; }
async_client& async_client::set_basic_auth(const std::string& u, const std::string& p) {
    impl_->has_basic = true;
    impl_->user = u;
    impl_->password = p;
    return *this;
}
async_client& async_client::set_bearer_token(const std::string& token) { impl_->bearer = token; return *this; }
async_client& async_client::set_verify(bool enable) { impl_->verify = enable; return *this; }
async_client& async_client::set_ca_file(const std::string& path) { impl_->ca_file = path; return *this; }

} // namespace httpp
