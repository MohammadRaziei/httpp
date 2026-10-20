#include "httpp/client.hpp"
#include "httpp/url.hpp"

// httplib.h (+ mbedtls support) is included ONLY via this internal header,
// never in a public httpp/*.hpp header - see its comment for why.
#include "internal/httplib_common.hpp"
#include "internal/blocking_pool.hpp"
#include "internal/async_http.hpp"
#include "internal/content_decode.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
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
void fill(response& out, httplib::Result& res, clock_type::time_point started, long timeout_seconds,
          std::size_t max_response_size = 0) {
    if (res) {
        out.status = res->status;
        out.body = res->body;
        for (const auto& [key, value] : res->headers) out.headers.emplace_back(key, value);
        detail::decode_content(out, max_response_size); // cpp-httplib is built without zlib: it passes the raw body
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
        cli.set_decompress(false); // we decode gzip / deflate ourselves (no system zlib), see content_decode.hpp
        defaults.emplace("Accept-Encoding", detail::default_accept_encoding);
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
    cli.set_decompress(false);
    if (!hdrs.count("Accept-Encoding")) hdrs.emplace("Accept-Encoding", detail::default_accept_encoding);

    httplib::Result res;
    const auto started = clock_type::now();
    if (!dispatch(cli, upper(r.method.empty() ? "GET" : r.method), path, hdrs, r.body, r.content_type, res)) {
        fail(out, error_kind::unsupported_method, "unsupported HTTP method: " + r.method);
        return out;
    }
    fill(out, res, started, r.timeout_seconds, r.max_response_size);
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

// --- the async path: requests run on the non-blocking engine (internal/async_http.hpp) ---

namespace {

constexpr int redirect_limit = 20; // same as cpp-httplib
constexpr long default_async_timeout = 300; // cpp-httplib's default connect/read timeout; 0 would mean "wait forever"

// One logical request. A redirect makes the next hop out of it.
struct async_spec {
    std::string method; // upper case
    std::string scheme, host;
    int port = 80;
    std::string target; // "/path?query", percent-encoded
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body, content_type;
    long timeout = 0;
    bool follow = false;
    bool verify = true;
    std::string ca_file;
    std::string proxy_host;
    int proxy_port = 0;
    std::size_t max_response_size = 0;
    int redirects_left = redirect_limit;
};

bool header_present(const async_spec& s, const char* name) {
    for (const auto& h : s.headers) {
        if (h.first.size() == std::strlen(name) &&
            std::equal(h.first.begin(), h.first.end(), name, [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }))
            return true;
    }
    return false;
}

void drop_header(async_spec& s, const char* name) {
    s.headers.erase(std::remove_if(s.headers.begin(), s.headers.end(), [name](const std::pair<std::string, std::string>& h) {
        return h.first.size() == std::strlen(name) &&
               std::equal(h.first.begin(), h.first.end(), name, [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
    }), s.headers.end());
}

std::string basic_auth_value(const std::string& user, const std::string& password) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const std::string in = user + ":" + password;
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
    }
    return out;
}

bool supported_method(const std::string& m) {
    return m == "GET" || m == "HEAD" || m == "OPTIONS" || m == "POST" || m == "PUT" || m == "PATCH" || m == "DELETE";
}

bool method_has_body(const std::string& m) { return m == "POST" || m == "PUT" || m == "PATCH" || m == "DELETE"; }

// The engine job for one hop of `s`.
detail::async_job make_job(const async_spec& s) {
    detail::async_job j;
    j.host = s.host;
    j.port = s.port;
    j.tls = s.scheme == "https";
    j.method = s.method;
    j.target = s.target;
    j.headers = s.headers;
    if (!header_present(s, "Accept")) j.headers.emplace_back("Accept", "*/*");
    if (!header_present(s, "User-Agent")) j.headers.emplace_back("User-Agent", "httpp");
    if (!header_present(s, "Accept-Encoding")) j.headers.emplace_back("Accept-Encoding", detail::default_accept_encoding);
    if (method_has_body(s.method)) {
        j.body = s.body;
        const std::string ctype = s.content_type.empty() ? "application/x-www-form-urlencoded" : s.content_type;
        if (!header_present(s, "Content-Type")) j.headers.emplace_back("Content-Type", ctype);
    }
    j.timeout_sec = s.timeout > 0 ? s.timeout : default_async_timeout;
    j.max_response_size = s.max_response_size;
    j.verify = s.verify;
    j.ca_file = s.ca_file;
    j.keep_alive = true; // pooled in the shared engine
    j.proxy_host = s.proxy_host;
    j.proxy_port = s.proxy_port;
    return j;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Resolves a Location header against the hop that produced it. False: not something to follow.
bool resolve_location(const async_spec& cur, std::string loc, async_spec& next) {
    const auto hash = loc.find('#');
    if (hash != std::string::npos) loc.resize(hash);
    if (loc.empty()) return false;
    next = cur;
    auto adopt = [&](const url& u) {
        if (!u.valid()) return false;
        next.scheme = u.scheme();
        next.host = u.host();
        next.port = u.port();
        next.target = u.path().empty() ? "/" : u.path();
        if (!u.query().empty()) next.target += "?" + u.query();
        return true;
    };
    const std::string low = lower(loc.substr(0, 8));
    if (low.compare(0, 7, "http://") == 0 || low.compare(0, 8, "https://") == 0) return adopt(url::parse(loc));
    if (loc.find("://") != std::string::npos && loc.find('/') > loc.find("://")) return false; // ftp:// and friends
    if (loc.compare(0, 2, "//") == 0) return adopt(url::parse(cur.scheme + ":" + loc));
    if (loc[0] == '/') { next.target = loc; return true; }
    const std::string cur_path = cur.target.substr(0, cur.target.find('?'));
    if (loc[0] == '?') { next.target = cur_path + loc; return true; }
    // relative path: directory of the current path + loc, with "." and ".." resolved
    std::string dir = cur_path.substr(0, cur_path.rfind('/') + 1);
    std::string joined = dir + loc, query;
    const auto q = joined.find('?');
    if (q != std::string::npos) { query = joined.substr(q); joined.resize(q); }
    std::vector<std::string> out;
    std::size_t pos = 1;
    while (pos <= joined.size()) {
        std::size_t end = joined.find('/', pos);
        if (end == std::string::npos) end = joined.size();
        const std::string seg = joined.substr(pos, end - pos);
        if (seg == "..") { if (!out.empty()) out.pop_back(); }
        else if (seg != ".") out.push_back(seg);
        pos = end + 1;
    }
    std::string path = "/";
    for (std::size_t i = 0; i < out.size(); ++i) path += (i ? "/" : "") + out[i];
    next.target = path + query;
    return true;
}

bool same_origin(const async_spec& a, const async_spec& b) {
    return a.scheme == b.scheme && a.host == b.host && a.port == b.port;
}

// Runs `spec` on the shared engine and follows redirects if asked. `done` runs on the loop thread,
// once, with the final response.
void run_chain(std::shared_ptr<async_spec> spec, std::function<void(response)> done) {
    detail::shared_async_http().submit(make_job(*spec), [spec, done = std::move(done)](response r) mutable {
        const bool redirect = r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308;
        if (!spec->follow || !redirect) return done(std::move(r));
        std::string location;
        for (const auto& [name, value] : r.headers)
            if (lower(name) == "location") { location = value; break; }
        async_spec next;
        if (!resolve_location(*spec, location, next)) return done(std::move(r)); // nothing usable: the 3xx is the answer
        if (spec->redirects_left <= 0) {
            response out;
            out.error = error_kind::too_many_redirects;
            out.error_message = "too many redirects";
            return done(std::move(out));
        }
        next.redirects_left = spec->redirects_left - 1;
        if (!same_origin(*spec, next)) { // never hand credentials to another origin
            drop_header(next, "Authorization");
            drop_header(next, "Cookie");
            drop_header(next, "Proxy-Authorization");
        }
        if (r.status == 303 && next.method != "GET" && next.method != "HEAD") { // like cpp-httplib: 303 becomes a GET
            next.method = "GET";
            next.body.clear();
            next.content_type.clear();
            drop_header(next, "Content-Type");
            drop_header(next, "Content-Length");
        }
        run_chain(std::make_shared<async_spec>(std::move(next)), std::move(done));
    });
}

response failed_response(error_kind kind, std::string message) {
    response out;
    fail(out, kind, std::move(message));
    return out;
}

} // namespace

std::future<response> client::request::run_async() const {
    auto promise = std::make_shared<std::promise<response>>();
    auto future = promise->get_future();
    run_async([promise](response out) { promise->set_value(std::move(out)); });
    return future;
}

void client::request::run_async(std::function<void(response)> done) const {
    const impl& r = *impl_;

    const url u = url::parse(r.url);
    if (!u.valid()) {
        done(failed_response(error_kind::invalid_url, "not a usable http(s) URL: " + r.url));
        return;
    }
    auto spec = std::make_shared<async_spec>();
    spec->method = upper(r.method.empty() ? "GET" : r.method);
    if (!supported_method(spec->method)) {
        done(failed_response(error_kind::unsupported_method, "unsupported HTTP method: " + r.method));
        return;
    }
    spec->scheme = u.scheme();
    spec->host = u.host();
    spec->port = u.port();
    spec->target = u.path().empty() ? "/" : u.path();
    std::string query = u.query();
    if (!r.params.empty()) {
        if (!query.empty()) query += "&";
        query += url::build_query(r.params);
    }
    if (!query.empty()) spec->target += "?" + query;

    spec->headers = r.headers;
    if (!r.cookies.empty() && !header_present(*spec, "Cookie")) {
        std::string joined;
        for (const auto& [name, value] : r.cookies) {
            if (!joined.empty()) joined += "; ";
            joined += name + "=" + value;
        }
        spec->headers.emplace_back("Cookie", joined);
    }
    if (!header_present(*spec, "Authorization")) {
        if (r.has_basic_auth) spec->headers.emplace_back("Authorization", "Basic " + basic_auth_value(r.auth_user, r.auth_password));
        else if (!r.bearer.empty()) spec->headers.emplace_back("Authorization", "Bearer " + r.bearer);
    }
    spec->body = r.body;
    spec->content_type = r.content_type;
    spec->timeout = r.timeout_seconds;
    spec->follow = r.follow_redirects;
    spec->verify = r.verify;
    spec->ca_file = r.ca_file;
    spec->proxy_host = r.proxy_host;
    spec->proxy_port = r.proxy_port;
    spec->max_response_size = r.max_response_size;

    run_chain(spec, std::move(done));
}

// --- async_client: a base URL, shared defaults and at most `max_connections` requests at a time ---

struct async_client::impl {
    std::string scheme, host, base_path;
    int port = 80;
    std::size_t max_connections = 8;

    // settings applied to every request (configure before the first one)
    std::vector<std::pair<std::string, std::string>> headers;
    long timeout = 0;
    bool follow = false;
    bool has_basic = false;
    std::string user, password, bearer, ca_file;
    bool verify = true;
    std::string proxy_host;
    int proxy_port = 0;

    // at most max_connections requests in flight; the rest wait here
    struct waiting {
        std::shared_ptr<async_spec> spec;
        std::function<void(response)> done;
    };
    std::mutex mu;
    std::deque<waiting> queue;
    std::size_t in_flight = 0;

    void start(waiting w, const std::shared_ptr<impl>& self) {
        run_chain(w.spec, [self, done = std::move(w.done)](response out) {
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
    const url u = url::parse(base_url);
    if (!u.valid()) throw std::invalid_argument("httpp::async_client: not a usable http(s) URL: " + base_url);
    impl_->scheme = u.scheme();
    impl_->host = u.host();
    impl_->port = u.port();
    std::string p = u.path();
    while (!p.empty() && p.back() == '/') p.pop_back();
    impl_->base_path = p;
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

    auto spec = std::make_shared<async_spec>();
    spec->method = upper(method);
    if (!supported_method(spec->method)) {
        done(failed_response(error_kind::unsupported_method, "unsupported HTTP method: " + method));
        return;
    }
    spec->scheme = st.scheme;
    spec->host = st.host;
    spec->port = st.port;
    spec->target = join_path(st.base_path, path);
    spec->headers = st.headers;
    for (const auto& [name, value] : headers) { // a per-call header replaces a default of the same name
        drop_header(*spec, name.c_str());
        spec->headers.emplace_back(name, value);
    }
    if (!header_present(*spec, "Authorization")) {
        if (st.has_basic) spec->headers.emplace_back("Authorization", "Basic " + basic_auth_value(st.user, st.password));
        else if (!st.bearer.empty()) spec->headers.emplace_back("Authorization", "Bearer " + st.bearer);
    }
    spec->body = body;
    spec->content_type = content_type;
    spec->timeout = st.timeout;
    spec->follow = st.follow;
    spec->verify = st.verify;
    spec->ca_file = st.ca_file;
    spec->proxy_host = st.proxy_host;
    spec->proxy_port = st.proxy_port;

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
async_client& async_client::set_proxy(const std::string& host, int port) {
    impl_->proxy_host = host;
    impl_->proxy_port = port;
    return *this;
}
async_client& async_client::set_verify(bool enable) { impl_->verify = enable; return *this; }
async_client& async_client::set_ca_file(const std::string& path) { impl_->ca_file = path; return *this; }

} // namespace httpp
