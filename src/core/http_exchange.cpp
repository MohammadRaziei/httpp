#include "internal/http_exchange.hpp"

#include "httpp/url.hpp"
#include "internal/content_decode.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <future>

namespace httpp::detail {

// --- small value helpers ---

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

bool supported_method(const std::string& m) {
    return m == "GET" || m == "HEAD" || m == "OPTIONS" || m == "POST" || m == "PUT" || m == "PATCH" || m == "DELETE";
}

bool method_has_body(const std::string& m) { return m == "POST" || m == "PUT" || m == "PATCH" || m == "DELETE"; }

std::string join_path(const std::string& base, const std::string& path) {
    if (path.empty()) return base.empty() ? "/" : base;
    return base + (path[0] == '/' ? "" : "/") + path;
}

response failed_response(error_kind kind, std::string message) {
    response out;
    out.status = 0;
    out.error = kind;
    out.error_message = std::move(message);
    return out;
}

bool has_header(const header_list& headers, const std::string& name) {
    return std::any_of(headers.begin(), headers.end(), [&](const auto& h) { return iequals(h.first, name); });
}

void remove_header(header_list& headers, const std::string& name) {
    headers.erase(std::remove_if(headers.begin(), headers.end(), [&](const auto& h) { return iequals(h.first, name); }), headers.end());
}

void set_header(header_list& headers, std::string name, std::string value) {
    remove_header(headers, name);
    headers.emplace_back(std::move(name), std::move(value));
}

namespace {

std::string base64(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
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

} // namespace

// --- session_settings ---

void session_settings::set_default_header(std::string name, std::string value) { set_header(headers, std::move(name), std::move(value)); }

void session_settings::set_basic(std::string user_name, std::string pass) {
    has_basic = true;
    user = std::move(user_name);
    password = std::move(pass);
}

void session_settings::set_bearer(std::string token) {
    bearer = std::move(token); // basic wins if both are set
}

bool session_settings::make_spec(const endpoint& where, const std::string& method, const std::string& path,
                                 const header_list& call_headers, std::string body_bytes, std::string ctype,
                                 request_spec& out, response& error) const {
    out = request_spec{};
    out.method = to_upper(method.empty() ? "GET" : method);
    if (!supported_method(out.method)) {
        error = failed_response(error_kind::unsupported_method, "unsupported HTTP method: " + method);
        return false;
    }
    out.scheme = where.scheme;
    out.host = where.host;
    out.port = where.port;
    out.target = join_path(where.base_path, path);

    out.headers = headers;
    for (const auto& h : call_headers) remove_header(out.headers, h.first); // a call header replaces a default of the same name
    out.headers.insert(out.headers.end(), call_headers.begin(), call_headers.end());
    if (!has_header(out.headers, "Authorization")) {
        if (has_basic) out.headers.emplace_back("Authorization", "Basic " + base64(user + ":" + password));
        else if (!bearer.empty()) out.headers.emplace_back("Authorization", "Bearer " + bearer);
    }

    out.body = std::move(body_bytes);
    out.content_type = std::move(ctype);
    out.timeout = timeout;
    out.follow = follow;
    out.verify = verify;
    out.ca_file = ca_file;
    out.proxy_host = proxy_host;
    out.proxy_port = proxy_port;
    out.max_response_size = max_response_size;
    out.keep_alive = keep_alive;
    return true;
}

// --- one hop, and the redirect loop ---

async_job make_job(const request_spec& s) {
    async_job j;
    j.host = s.host;
    j.port = s.port;
    j.tls = s.scheme == "https";
    j.method = s.method;
    j.target = s.target;
    j.headers = s.headers;
    if (!has_header(s.headers, "Accept")) j.headers.emplace_back("Accept", "*/*");
    if (!has_header(s.headers, "User-Agent")) j.headers.emplace_back("User-Agent", "httpp");
    if (!has_header(s.headers, "Accept-Encoding")) j.headers.emplace_back("Accept-Encoding", default_accept_encoding);
    if (method_has_body(s.method)) {
        j.body = s.body;
        if (!has_header(s.headers, "Content-Type"))
            j.headers.emplace_back("Content-Type", s.content_type.empty() ? "application/x-www-form-urlencoded" : s.content_type);
    }
    j.timeout_sec = s.timeout > 0 ? s.timeout : request_spec::default_timeout;
    j.max_response_size = s.max_response_size;
    j.verify = s.verify;
    j.ca_file = s.ca_file;
    j.keep_alive = s.keep_alive;
    j.proxy_host = s.proxy_host;
    j.proxy_port = s.proxy_port;
    return j;
}

bool resolve_location(const request_spec& cur, std::string loc, request_spec& next) {
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
    const std::string low = to_lower(loc.substr(0, 8));
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

namespace {

bool same_origin(const request_spec& a, const request_spec& b) {
    return a.scheme == b.scheme && a.host == b.host && a.port == b.port;
}

bool is_redirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

std::string location_of(const response& r) {
    for (const auto& [name, value] : r.headers)
        if (iequals(name, "location")) return value;
    return {};
}

} // namespace

void run_chain(std::shared_ptr<request_spec> spec, std::function<void(response)> done) {
    shared_async_http().submit(make_job(*spec), [spec, done = std::move(done)](response r) mutable {
        if (!spec->follow || !is_redirect(r.status)) return done(std::move(r));
        request_spec next;
        if (!resolve_location(*spec, location_of(r), next)) return done(std::move(r)); // nothing usable: the 3xx is the answer
        if (spec->redirects_left <= 0) return done(failed_response(error_kind::too_many_redirects, "too many redirects"));
        next.redirects_left = spec->redirects_left - 1;
        if (!same_origin(*spec, next)) { // never hand credentials to another origin
            remove_header(next.headers, "Authorization");
            remove_header(next.headers, "Cookie");
            remove_header(next.headers, "Proxy-Authorization");
        }
        if (r.status == 303 && next.method != "GET" && next.method != "HEAD") { // 303 becomes a GET
            next.method = "GET";
            next.body.clear();
            next.content_type.clear();
            remove_header(next.headers, "Content-Type");
            remove_header(next.headers, "Content-Length");
        }
        run_chain(std::make_shared<request_spec>(std::move(next)), std::move(done));
    });
}

response run_blocking(std::shared_ptr<request_spec> spec) {
    if (shared_async_http().on_loop_thread())
        return failed_response(error_kind::other, "blocking call made on the network thread (inside an async callback); use the async API there");
    auto promise = std::make_shared<std::promise<response>>();
    auto future = promise->get_future();
    run_chain(std::move(spec), [promise](response out) { promise->set_value(std::move(out)); });
    return future.get();
}

} // namespace httpp::detail
