// The public client classes. They are thin: what to send and which redirects to follow is
// internal/http_exchange.cpp (shared with downloads later), moving bytes is the engine
// (internal/async_http.cpp). Here: base URL handling, the fluent builders and the sync/async calling styles.
#include "httpp/client.hpp"
#include "httpp/url.hpp"

#include "internal/http_exchange.hpp"

#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace httpp {
namespace {

using detail::endpoint;
using detail::endpoint_of;
using detail::header_list;
using detail::request_spec;
using detail::session_settings;

endpoint parse_base(const std::string& base_url, const char* who) {
    const url u = url::parse(base_url);
    if (!u.valid()) throw std::invalid_argument(std::string(who) + ": not a usable http(s) URL: " + base_url);
    return endpoint_of(u, true);
}

// Settings under a mutex: a session can be shared between threads, and a request uses the settings
// it started with (the spec is a copy).
class guarded_settings {
public:
    template <class F> void update(F&& f) {
        std::lock_guard<std::mutex> lock(mu_);
        f(settings_);
    }
    bool make_spec(const endpoint& where, const std::string& method, const std::string& path, const header_list& headers,
                   std::string body, std::string content_type, request_spec& out, response& error) const {
        std::lock_guard<std::mutex> lock(mu_);
        return settings_.make_spec(where, method, path, headers, std::move(body), std::move(content_type), out, error);
    }

private:
    mutable std::mutex mu_;
    session_settings settings_;
};

} // namespace

// --- client (a keep-alive session; calls block, over the shared engine) ---

struct client::impl {
    endpoint where;
    guarded_settings settings;
    explicit impl(endpoint e) : where(std::move(e)) {}
};

client::client(const std::string& host, int port, bool use_ssl) {
    endpoint e;
    e.scheme = use_ssl ? "https" : "http";
    e.host = host;
    e.port = port;
    impl_ = std::make_unique<impl>(std::move(e));
}

client::client(const std::string& base_url) : impl_(std::make_unique<impl>(parse_base(base_url, "httpp::client"))) {}

client::~client() = default;
client::client(client&&) noexcept = default;
client& client::operator=(client&&) noexcept = default;

response client::send(const std::string& method, const std::string& path, const headers_t& headers,
                      const std::string& body, const std::string& content_type) {
    request_spec spec;
    response error;
    if (!impl_->settings.make_spec(impl_->where, method, path, headers, body, content_type, spec, error)) return error;
    return detail::run_blocking(std::make_shared<request_spec>(std::move(spec)));
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
    impl_->settings.update([&](session_settings& s) { s.set_default_header(std::move(name), std::move(value)); });
    return *this;
}
client& client::set_timeout(long seconds) {
    impl_->settings.update([&](session_settings& s) { s.set_timeout(seconds); });
    return *this;
}
client& client::set_follow_redirects(bool enable) {
    impl_->settings.update([&](session_settings& s) { s.follow = enable; });
    return *this;
}
client& client::set_basic_auth(const std::string& user, const std::string& password) {
    impl_->settings.update([&](session_settings& s) { s.set_basic(user, password); });
    return *this;
}
client& client::set_bearer_token(const std::string& token) {
    impl_->settings.update([&](session_settings& s) { s.set_bearer(token); });
    return *this;
}
client& client::set_proxy(const std::string& host, int port) {
    impl_->settings.update([&](session_settings& s) { s.proxy_host = host; s.proxy_port = port; });
    return *this;
}
client& client::set_verify(bool enable) {
    impl_->settings.update([&](session_settings& s) { s.verify = enable; });
    return *this;
}
client& client::set_ca_file(const path& file) {
    impl_->settings.update([&](session_settings& s) { s.ca_file = file.str(); });
    return *this;
}
client& client::set_keep_alive(bool enable) {
    impl_->settings.update([&](session_settings& s) { s.keep_alive = enable; });
    return *this;
}

response client::fetch(const std::string& full_url) {
    if (!url::parse(full_url).valid())
        throw std::invalid_argument("httpp::client::fetch: unsupported or invalid URL: " + full_url);
    session_settings once;
    once.keep_alive = false; // one request: do not park the connection
    request_spec spec;
    response error;
    if (!once.make_spec_for_url(full_url, "", "GET", {}, {}, {}, spec, error)) return error;
    return detail::run_blocking(std::make_shared<request_spec>(std::move(spec)));
}

// --- request (fluent builder; also what curl_compat.cpp is built on) ---

struct request::impl {
    std::string url;
    std::string method;       // empty until set explicitly or via a body
    std::string body;
    std::string content_type = "application/x-www-form-urlencoded";
    header_list headers;      // repeatable, in the order given
    header_list params, cookies;
    session_settings settings; // timeout, redirects, auth, proxy, TLS, size limit

    explicit impl(std::string u) : url(std::move(u)) {}

    // The spec for this request. False (and `error` set) when it cannot even start.
    bool make_spec(request_spec& out, response& error) const {
        header_list call = headers;
        if (!cookies.empty() && !detail::has_header(call, "Cookie")) {
            std::string joined;
            for (const auto& [name, value] : cookies) {
                if (!joined.empty()) joined += "; ";
                joined += name + "=" + value;
            }
            call.emplace_back("Cookie", std::move(joined));
        }
        return settings.make_spec_for_url(url, params.empty() ? std::string() : url::build_query(params), method, call, body,
                                          content_type, out, error);
    }
};

request::request(std::string url) : impl_(std::make_unique<impl>(std::move(url))) {}
request::~request() = default;
request::request(request&&) noexcept = default;
request& request::operator=(request&&) noexcept = default;

request& request::method(std::string m) { impl_->method = std::move(m); return *this; }
request& request::header(std::string name, std::string value) {
    impl_->headers.emplace_back(std::move(name), std::move(value));
    return *this;
}
request& request::param(std::string name, std::string value) {
    impl_->params.emplace_back(std::move(name), std::move(value));
    return *this;
}
request& request::data(std::string body) {
    impl_->body = std::move(body);
    if (impl_->method.empty()) impl_->method = "POST";
    return *this;
}
request& request::json(std::string document) {
    impl_->body = std::move(document);
    impl_->content_type = "application/json";
    if (impl_->method.empty()) impl_->method = "POST";
    return *this;
}
request& request::form(const std::vector<std::pair<std::string, std::string>>& fields) {
    impl_->body = url::build_query(fields);
    impl_->content_type = "application/x-www-form-urlencoded";
    if (impl_->method.empty()) impl_->method = "POST";
    return *this;
}
request& request::content_type(std::string type) { impl_->content_type = std::move(type); return *this; }
request& request::basic_auth(const std::string& user, const std::string& password) {
    impl_->settings.set_basic(user, password);
    return *this;
}
request& request::bearer(const std::string& token) { impl_->settings.set_bearer(token); return *this; }
request& request::cookie(std::string name, std::string value) {
    impl_->cookies.emplace_back(std::move(name), std::move(value));
    return *this;
}
request& request::timeout(long seconds) { impl_->settings.timeout = seconds; return *this; }
request& request::follow_redirects(bool enable) { impl_->settings.follow = enable; return *this; }
request& request::verify(bool enable) { impl_->settings.verify = enable; return *this; }
request& request::ca_file(const path& file) { impl_->settings.ca_file = file.str(); return *this; }
request& request::proxy(std::string host, int port) {
    impl_->settings.proxy_host = std::move(host);
    impl_->settings.proxy_port = port;
    return *this;
}
request& request::max_response_size(std::size_t bytes) { impl_->settings.max_response_size = bytes; return *this; }

response request::run() const {
    request_spec spec;
    response error;
    if (!impl_->make_spec(spec, error)) return error;
    return detail::run_blocking(std::make_shared<request_spec>(std::move(spec)));
}

std::future<response> request::run_async() const {
    auto promise = std::make_shared<std::promise<response>>();
    auto future = promise->get_future();
    run_async([promise](response out) { promise->set_value(std::move(out)); });
    return future;
}

void request::run_async(std::function<void(response)> done) const {
    auto spec = std::make_shared<request_spec>();
    response error;
    if (!impl_->make_spec(*spec, error)) {
        done(std::move(error));
        return;
    }
    detail::run_chain(std::move(spec), std::move(done));
}

// --- async_client: a base URL, shared defaults and at most `max_connections` requests at a time ---

struct async_client::impl {
    endpoint where;
    session_settings settings; // configure before the first request
    std::size_t max_connections = 8;

    // at most max_connections requests in flight; the rest wait here
    struct waiting {
        std::shared_ptr<request_spec> spec;
        std::function<void(response)> done;
    };
    std::mutex mu;
    std::deque<waiting> queue;
    std::size_t in_flight = 0;

    void start(waiting w, const std::shared_ptr<impl>& self) {
        detail::run_chain(w.spec, [self, done = std::move(w.done)](response out) {
            done(std::move(out));
            self->finished(self);
        });
    }

    void finished(const std::shared_ptr<impl>& self) {
        waiting next;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (queue.empty()) { --in_flight; return; }
            next = std::move(queue.front());
            queue.pop_front();
        }
        start(std::move(next), self); // the freed slot goes straight to the next waiting request
    }
};

async_client::async_client(const std::string& base_url, std::size_t max_connections)
    : impl_(std::make_shared<impl>()) {
    impl_->where = parse_base(base_url, "httpp::async_client");
    impl_->max_connections = max_connections == 0 ? 1 : max_connections;
}
async_client::~async_client() = default;
async_client::async_client(async_client&&) noexcept = default;
async_client& async_client::operator=(async_client&&) noexcept = default;

std::future<response> async_client::send(const std::string& method, const std::string& path,
                                         const headers_t& headers, const std::string& body,
                                         const std::string& content_type) {
    auto promise = std::make_shared<std::promise<response>>();
    auto future = promise->get_future();
    send(method, path, headers, body, content_type, [promise](response out) { promise->set_value(std::move(out)); });
    return future;
}

void async_client::send(const std::string& method, const std::string& path, const headers_t& headers,
                        const std::string& body, const std::string& content_type,
                        std::function<void(response)> done) {
    auto& st = *impl_;
    auto spec = std::make_shared<request_spec>();
    response error;
    if (!st.settings.make_spec(st.where, method, path, headers, body, content_type, *spec, error)) {
        done(std::move(error));
        return;
    }

    impl::waiting w{spec, std::move(done)};
    {
        std::lock_guard<std::mutex> lock(st.mu);
        if (st.in_flight >= st.max_connections) {
            st.queue.push_back(std::move(w));
            return;
        }
        ++st.in_flight;
    }
    st.start(std::move(w), impl_);
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
    impl_->settings.set_default_header(std::move(name), std::move(value));
    return *this;
}
async_client& async_client::set_timeout(long seconds) { impl_->settings.timeout = seconds; return *this; }
async_client& async_client::set_follow_redirects(bool enable) { impl_->settings.follow = enable; return *this; }
async_client& async_client::set_basic_auth(const std::string& u, const std::string& p) {
    impl_->settings.set_basic(u, p);
    return *this;
}
async_client& async_client::set_bearer_token(const std::string& token) { impl_->settings.set_bearer(token); return *this; }
async_client& async_client::set_proxy(const std::string& host, int port) {
    impl_->settings.proxy_host = host;
    impl_->settings.proxy_port = port;
    return *this;
}
async_client& async_client::set_verify(bool enable) { impl_->settings.verify = enable; return *this; }
async_client& async_client::set_ca_file(const path& file) { impl_->settings.ca_file = file.str(); return *this; }

} // namespace httpp
