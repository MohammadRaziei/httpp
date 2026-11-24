#include "internal/http_exchange.hpp"

#include "httpp/url.hpp"
#include "internal/content_decode.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <future>
#include <stdexcept>

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

bool is_proxy_direct_url(const url& u) {
    return u.protocol() == "direct" && u.host_text().empty() && u.port() == 0 && u.username().empty() && u.password().empty() &&
           u.query().empty() && u.fragment().empty() && (u.path().empty() || u.path() == "/");
}

proxy_setting parse_proxy_url(const url& u) {
    proxy_setting out;
    if (u.str().empty() || is_proxy_direct_url(u)) return out;
    // not str(): for direct:// it is normalized into something the user never wrote ("direct:somehost/")
    if (u.protocol() == "direct")
        throw std::invalid_argument("proxy: direct:// turns the proxy off and takes nothing after the scheme (no host, port, user, path, query or fragment)");
    if (!url_valid(u) || u.protocol() == "https")
        throw std::invalid_argument("proxy: '" + u.str() + "' is not an http://, socks5:// or socks5h:// URL with a host");
    const std::string user = url_username(u), pass = url_password(u);
    out.host = url_host(u);
    out.port = u.effective_port();
    if (u.protocol() == "http") {
        if (!user.empty() || !pass.empty()) out.authorization = "Basic " + base64(user + ":" + pass);
    } else {
        out.kind = u.protocol() == "socks5h" ? proxy_kind::socks5h : proxy_kind::socks5;
        if (user.size() > 255 || pass.size() > 255)
            throw std::invalid_argument("proxy: a SOCKS5 user name and password are at most 255 bytes each");
        out.user = user;
        out.password = pass;
    }
    return out;
}

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

namespace {
// A wait given in seconds: a negative number or not-a-number throws (so does one above `most`).
void check_wait(double seconds, const char* name, double most) {
    if (!(seconds >= 0)) throw std::invalid_argument(std::string(name) + " must not be negative"); // not a number fails this too
    if (seconds > most) throw std::invalid_argument(std::string(name) + " is too long");
}
} // namespace

void session_settings::set_connect_timeout(double seconds) {
    check_wait(seconds, "connect_timeout", longest_wait_seconds);
    connect_timeout = seconds;
}

void session_settings::set_max_time(double seconds) {
    check_wait(seconds, "max_time", longest_wait_seconds);
    max_time = seconds;
}

void session_settings::set_retry(int times) {
    if (times < 0) throw std::invalid_argument("retry must not be negative");
    retries = times;
}

void session_settings::set_retry_delay(double seconds) {
    check_wait(seconds, "retry_delay", 86400); // a day, as the command line has always limited it
    retry_delay = seconds;
}

void session_settings::set_max_redirects(int count) {
    const int hops = redirect_hops(count); // throws before anything changes
    follow = count != 0;
    max_redirects = hops;
}

int session_settings::redirects() const { return follow ? redirect_count(max_redirects) : 0; }

void session_settings::set_proxy(const url& proxy_url) {
    proxy = parse_proxy_url(proxy_url); // throws before anything changes
}

std::string endpoint_url(const endpoint& where, const std::string& path) {
    std::string out = where.scheme + "://";
    if (where.host.find(':') != std::string::npos && where.host.front() != '[') out += "[" + where.host + "]";
    else out += where.host;
    out += ":" + std::to_string(where.port);
    out += join_path(where.base_path, path);
    return out;
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
    out.connect_timeout = connect_timeout;
    out.max_time = max_time;
    out.retries = retries;
    out.retry_delay = retry_delay;
    out.retry_connrefused = retry_connrefused;
    out.on_retry = on_retry;
    out.redirects_left = max_redirects;
    out.follow = follow;
    out.auto_compression = auto_compression;
    out.verify = verify;
    out.ca_file = ca_file;
    out.proxy = proxy;
    out.max_response_size = max_response_size;
    out.keep_alive = keep_alive;
    return true;
}

endpoint endpoint_of(const url& u, bool with_base_path) {
    endpoint e;
    e.scheme = url_scheme(u);
    e.host = url_host(u);
    e.port = u.effective_port();
    if (with_base_path) {
        std::string p = url_path(u);
        while (!p.empty() && p.back() == '/') p.pop_back();
        e.base_path = std::move(p);
    }
    return e;
}

bool session_settings::make_spec_for_url(const std::string& full_url, const std::string& extra_query,
                                         const std::string& method, const header_list& call_headers,
                                         std::string body_bytes, std::string ctype, request_spec& out,
                                         response& error) const {
    const url u = parse_url(full_url);
    if (!url_is_http(u)) {
        error = failed_response(error_kind::invalid_url, "not a usable http(s) URL: " + full_url);
        return false;
    }
    std::string query = url_query(u);
    if (!extra_query.empty()) query += (query.empty() ? "" : "&") + extra_query;
    std::string target = url_path(u);
    if (!query.empty()) target += "?" + query;
    return make_spec(endpoint_of(u, false), method, target, call_headers, std::move(body_bytes), std::move(ctype), out, error);
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
    if (s.auto_compression && !has_header(s.headers, "Accept-Encoding")) j.headers.emplace_back("Accept-Encoding", default_accept_encoding);
    j.auto_decode = s.auto_compression;
    if (method_has_body(s.method)) {
        j.body = s.body;
        if (!has_header(s.headers, "Content-Type"))
            j.headers.emplace_back("Content-Type", s.content_type.empty() ? "application/x-www-form-urlencoded" : s.content_type);
    }
    j.timeout_sec = s.timeout > 0 ? s.timeout : request_spec::default_timeout;
    j.connect_timeout_sec = s.connect_timeout;
    j.deadline = s.deadline;
    j.max_response_size = s.max_response_size;
    j.verify = s.verify;
    j.ca_file = s.ca_file;
    j.keep_alive = s.keep_alive;
    j.proxy_host = s.proxy.host;
    j.proxy_port = s.proxy.port;
    j.proxy_authorization = s.proxy.authorization;
    j.proxy_type = s.proxy.kind;
    j.proxy_user = s.proxy.user;
    j.proxy_password = s.proxy.password;
    if (s.sink) { // a redirect we follow is not for the sink: its body is thrown away
        auto inner = s.sink;
        const bool follow = s.follow;
        j.sink = std::make_shared<body_sink>();
        j.sink->on_head = [inner, follow](const response& head) {
            return follow && is_redirect(head.status) ? body_sink::verdict::skip : inner->on_head(head);
        };
        j.sink->on_data = inner->on_data;
    }
    return j;
}

bool resolve_location(const request_spec& cur, std::string loc, request_spec& next) {
    const auto hash = loc.find('#');
    if (hash != std::string::npos) loc.resize(hash);
    if (loc.empty()) return false;
    next = cur;
    auto adopt = [&](const url& u) {
        if (!url_is_http(u)) return false;
        next.scheme = url_scheme(u);
        next.host = url_host(u);
        next.port = u.effective_port();
        next.target = url_path(u);
        if (!u.query().empty()) next.target += "?" + url_query(u);
        return true;
    };
    const std::string low = to_lower(loc.substr(0, 8));
    if (low.compare(0, 7, "http://") == 0 || low.compare(0, 8, "https://") == 0) return adopt(parse_url(loc));
    if (loc.find("://") != std::string::npos && loc.find('/') > loc.find("://")) return false; // ftp:// and friends
    if (loc.compare(0, 2, "//") == 0) return adopt(parse_url(cur.scheme + ":" + loc));
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

bool is_redirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

async_http& engine_for(const request_spec& spec) { return spec.sink ? download_async_http() : shared_async_http(); }

namespace {

bool same_origin(const request_spec& a, const request_spec& b) {
    return a.scheme == b.scheme && a.host == b.host && a.port == b.port;
}

std::string location_of(const response& r) {
    for (const auto& [name, value] : r.headers)
        if (iequals(name, "location")) return value;
    return {};
}

// One hop and, through the callback, the hops that follow it. The caller of run_chain() has already started the clock.
void run_hops(std::shared_ptr<request_spec> spec, std::function<void(response)> done) {
    if (spec->max_time > 0 && spec->deadline == std::chrono::steady_clock::time_point{}) // the first hop starts the clock; a redirect inherits it
        spec->deadline = std::chrono::steady_clock::now() + seconds_to_ms(spec->max_time);
    engine_for(*spec).submit(make_job(*spec), [spec, done = std::move(done)](response r) mutable {
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
        run_hops(std::make_shared<request_spec>(std::move(next)), std::move(done));
    });
}

} // namespace

namespace {

// One try: the clock of elapsed_ms and max_time starts here, so each try has its own.
void run_attempt(std::shared_ptr<request_spec> spec, std::function<void(response)> done) {
    const auto started = std::chrono::steady_clock::now();
    run_hops(std::move(spec), [started, done = std::move(done)](response r) mutable {
        r.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        done(std::move(r));
    });
}

// As in curl: a few statuses and a timeout may pass on another try; a failed connection only when asked.
bool transient_status(int status) {
    return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

bool worth_another_try(const request_spec& spec, const response& r) {
    if (r.failed()) return r.error == error_kind::timeout || (spec.retry_connrefused && r.error == error_kind::connection);
    return transient_status(r.status);
}

// Seconds: the asked-for wait, or 1, 2, 4, ... (600 at most).
double wait_before_retry(const request_spec& spec, int retried) {
    return spec.retry_delay > 0 ? spec.retry_delay : static_cast<double>(std::min<long>(600, 1L << std::min(retried, 9)));
}

// Try number `retried` + 1. `base` is never run itself: each try gets a copy, because a try writes its own deadline and its redirect count.
void run_tries(std::shared_ptr<const request_spec> base, int retried, std::function<void(response)> done) {
    auto spec = std::make_shared<request_spec>(*base);
    spec->deadline = {};
    if (spec->sink && retried < base->retries) { // an answer that is about to be sent again is read and thrown away, never shown
        auto inner = spec->sink;
        spec->sink = std::make_shared<body_sink>();
        spec->sink->on_head = [inner](const response& head) {
            return transient_status(head.status) ? body_sink::verdict::skip : inner->on_head(head);
        };
        spec->sink->on_data = inner->on_data;
    }
    run_attempt(std::move(spec), [base, retried, done = std::move(done)](response r) mutable {
        if (retried >= base->retries || !worth_another_try(*base, r)) return done(std::move(r));
        const double wait = wait_before_retry(*base, retried);
        if (base->on_retry) {
            try {
                base->on_retry(r, retried + 1, static_cast<long>(std::ceil(wait))); // whole seconds, rounded up: the notice's type is old (ADR-0097)
            } catch (...) { // a notice that throws must not strand the request on the network thread
            }
        }
        engine_for(*base).post_after(seconds_to_ms(wait), [base, retried, done = std::move(done)]() mutable {
            run_tries(base, retried + 1, std::move(done));
        });
    });
}

} // namespace

void run_chain(std::shared_ptr<request_spec> spec, std::function<void(response)> done) {
    if (spec->on_finish) {
        done = [finish = spec->on_finish, done = std::move(done)](response r) mutable {
            finish(r);
            done(std::move(r));
        };
    }
    if (spec->retries <= 0) return run_attempt(std::move(spec), std::move(done));
    run_tries(std::move(spec), 0, std::move(done));
}

response run_blocking(std::shared_ptr<request_spec> spec) {
    if (engine_for(*spec).on_loop_thread())
        return failed_response(error_kind::other, "blocking call made on the network thread (inside an async callback); use the async API there");
    auto promise = std::make_shared<std::promise<response>>();
    auto future = promise->get_future();
    run_chain(std::move(spec), [promise](response out) { promise->set_value(std::move(out)); });
    return future.get();
}

} // namespace httpp::detail
