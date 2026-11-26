#pragma once

// Internal, NOT installed. The client-side policy layer, shared by every public client class
// (httpp::client, httpp::request, httpp::async_client) and, later, by downloads (ADR-0051).
//
//   session_settings  what a caller configures once (headers, timeout, auth, proxy, TLS, redirects)
//   request_spec      one logical request, built from settings (Builder: session_settings::make_spec)
//   run_chain         runs a spec on the engine and follows redirects (the policy loop)
//   run_blocking      waits for run_chain (Adapter: sync on top of async)
//
// The engine (async_http.hpp) moves bytes; everything about *what* to send and which redirects to
// follow lives here, once. This header includes neither Asio nor mbedtls.

#include "httpp/client.hpp"
#include "httpp/url.hpp"
#include "async_http.hpp"

#include <limits>
#include <mutex>
#include <stdexcept>
#include <cstddef>
#include <functional>
#include <string_view>
#include <memory>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace httpp::detail {

// The proxy settings of one client or request, as parsed from "http://[user[:pass]@]host[:port]" (ADR-0067).
struct proxy_setting {
    std::string host;           // empty = no proxy
    int port = 0;
    proxy_kind kind = proxy_kind::http;
    std::string authorization;  // http proxy: "Basic ..." for the Proxy-Authorization header, or empty
    std::string user, password; // SOCKS5 proxy: RFC 1929 credentials, or empty
};
// direct:// (ADR-0106): an explicit proxy that means "no proxy at all", like "". A scheme and nothing else: no host, port, user,
// path (but "/"), query or fragment, so "direct://host" and "direct://:3128" are errors, not a proxy. The parser reads
// "direct:", "direct:/" and "direct://" the same, and so do we. It is judged on the parsed url, never on str(): that is
// normalized ("direct://" gives "direct:/"). A bare "direct" or "off" has no scheme and is a host, as in "http://direct".
bool is_proxy_direct_url(const url& u);
// An empty url (url("")) or url("direct") means no proxy. Accepts http://, socks5:// and socks5h:// with an optional user[:pass]@ and
// port. Throws std::invalid_argument for anything else ("host:port", "https://...", socks4) and for a SOCKS user or password over
// 255 bytes.
proxy_setting parse_proxy_url(const url& proxy);

using header_list = std::vector<std::pair<std::string, std::string>>;

// --- small value helpers ---

std::string to_upper(std::string s);
std::string to_lower(std::string s);
bool iequals(const std::string& a, const std::string& b);
bool supported_method(const std::string& upper_method);
bool method_has_body(const std::string& upper_method);
// "/base" + "path" -> "/base/path" ("/" when both are empty).
std::string join_path(const std::string& base, const std::string& path);
response failed_response(error_kind kind, std::string message);

// Header list helpers: names compare case-insensitively.
bool has_header(const header_list& headers, const std::string& name);
void remove_header(header_list& headers, const std::string& name);
void set_header(header_list& headers, std::string name, std::string value); // replace or add

// One logical request. A redirect makes the next hop out of it.
struct request_spec {
    static constexpr int redirect_limit = 20;
    static constexpr long default_timeout = 300; // 0 would mean "wait forever"
    // What run() collects into response::body unless max_response_size says otherwise (0 = no limit). A body held in memory costs about
    // three times its size while it is read and decoded, so a server that never stops sending (or a gzip bomb) must not be able to take
    // the whole machine; a transfer that streams (on_body, save_to, download) never meets this limit.
    static constexpr std::size_t default_max_response_size = 256u * 1024 * 1024;

    std::string method; // upper case
    std::string scheme, host;
    int port = 80;
    std::string target; // "/path?query", percent-encoded
    header_list headers;
    std::string body, content_type;
    // A body read from a file or built from parts while it is sent (ADR-0113). When set, `body` is empty and content_type carries the
    // type (multipart boundary included). Shared by the copies a redirect or a retry makes: every pass opens its own reader.
    std::shared_ptr<const body_source> source;
    double timeout = 0; // seconds
    double connect_timeout = 0; // seconds; 0 = timeout covers the connect steps too
    double max_time = 0; // seconds for the whole request, redirects included; 0 = no such limit
    std::chrono::steady_clock::time_point deadline{}; // set from max_time when the request starts; epoch = none
    int retries = 0; // more tries after one that may pass (ADR-0097); 0 = never
    double retry_delay = 0; // seconds before each new try; 0 = 1, 2, 4, ... up to 600
    bool retry_connrefused = false; // also try again after error_kind::connection
    std::function<void(const response&, int, long)> on_retry; // before each new try: the last response, its number, the wait
    std::function<void(response&)> on_finish; // once, with the final response, before it is handed to the caller (ADR-0097)
    bool follow = false;
    bool auto_compression = true; // send Accept-Encoding and decode gzip / deflate bodies
    bool verify = true;
    std::string ca_file;
    proxy_setting proxy;
    std::size_t max_response_size = request_spec::default_max_response_size;
    bool keep_alive = true; // keep the connection in the engine's pool for the next request
    // Stream the final response body to this sink (it runs on download_async_http()). Redirect and
    // error-status handling stays in the sink's on_head; a redirect that is followed is never offered to it.
    std::shared_ptr<body_sink> sink;
    int redirects_left = redirect_limit;
};

// Where a session sends its requests.
struct endpoint {
    std::string scheme = "http", host;
    int port = 80;
    std::string base_path; // prepended to every path, no trailing slash
};

// scheme, host and port of a parsed URL (and its path as the base path, without a trailing slash, for a base URL).
endpoint endpoint_of(const url& u, bool with_base_path);

// Everything a caller configures once and that applies to each request made afterwards.
struct session_settings {
    header_list headers;
    double timeout = 0; // seconds; 0 = default_timeout
    double connect_timeout = 0; // seconds; 0 = timeout covers the connect steps too
    double max_time = 0; // seconds; 0 = no limit on the whole request
    int retries = 0;
    double retry_delay = 0; // seconds; 0 = 1, 2, 4, ... up to 600
    bool retry_connrefused = false;
    std::function<void(const response&, int, long)> on_retry; // only a download's job sets this (download::on_retry, ADR-0109); a request sets its own on the spec
    int max_redirects = request_spec::redirect_limit;
    bool follow = false;
    bool auto_compression = true;
    bool verify = true;
    std::string ca_file;
    proxy_setting proxy;
    std::size_t max_response_size = request_spec::default_max_response_size;
    bool keep_alive = true;
    bool has_basic = false;
    std::string user, password, bearer;

    // The rules of the settings, once (ADR-0093): what a setter does beyond storing the value. They are not accessors
    // (the fields carry the names), so they keep the `set_` prefix.
    // The session default, replacing a same-name one (the session setter).
    void set_default_header(std::string name, std::string value);
    void set_basic(std::string user_name, std::string pass);
    void set_bearer(std::string token);
    // 0, less, or not a number: the default again; above longest_wait_seconds it is cut to that
    void set_timeout(double seconds) { timeout = seconds > 0 ? (seconds < longest_wait_seconds ? seconds : longest_wait_seconds) : 0; }
    void set_connect_timeout(double seconds); // throws std::invalid_argument for a negative number, not a number, or too long
    void set_max_time(double seconds);        // same
    void set_retry(int times);               // same
    void set_retry_delay(double seconds);    // same, and more than a day
    void set_max_redirects(int count);       // 0 follows none, -1 no limit, any other negative number throws (redirect_hops)
    int redirects() const;                   // what set_max_redirects was given: 0, n or -1
    void set_proxy(const url& proxy_url);    // "" = no proxy; throws std::invalid_argument for a URL that is no proxy

    // Builder: the spec for one request. `call_headers` replace same-name session headers. Credentials
    // from the settings are added unless a call header already carries Authorization. Returns false
    // (and `error` is set) for a method httpp does not support.
    bool make_spec(const endpoint& where, const std::string& method, const std::string& path,
                   const header_list& call_headers, std::string body, std::string content_type,
                   request_spec& out, response& error) const;

    // Same for a full URL: its path and query become the target, `extra_query` is appended to the query.
    // An unusable URL is error_kind::invalid_url.
    bool make_spec_for_url(const std::string& full_url, const std::string& extra_query, const std::string& method,
                           const header_list& call_headers, std::string body, std::string content_type,
                           request_spec& out, response& error) const;
};

bool is_redirect(int status);

// max_redirects(count) of the public API: 0 follows none, -1 has no limit, any other negative number is refused
// (std::invalid_argument). The engine counts hops down from a limit, so "no limit" is the largest int.
inline int redirect_hops(int count) {
    if (count < -1) throw std::invalid_argument("max_redirects must be -1 (no limit) or more");
    return count < 0 ? std::numeric_limits<int>::max() : count;
}
// The public value of an engine limit (what a getter returns).
inline int redirect_count(int hops) { return hops == std::numeric_limits<int>::max() ? -1 : hops; }

// A session's or a request's settings under a mutex (ADR-0093): a session can be shared between threads, and a request made from it
// uses the settings it started with (the spec is a copy). Every class of the client family holds one of these, so a setter
// and a getter are the same code for all of them.
class guarded_settings {
public:
    guarded_settings() = default;
    explicit guarded_settings(session_settings s) : s_(std::move(s)) {}
    guarded_settings(const guarded_settings&) = delete;
    guarded_settings& operator=(const guarded_settings&) = delete;

    session_settings snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return s_;
    }
    template <class F> void update(F&& f) {
        std::lock_guard<std::mutex> lock(mu_);
        f(s_);
    }
    template <class F> auto read(F&& f) const {
        std::lock_guard<std::mutex> lock(mu_);
        return f(s_);
    }
    bool make_spec(const endpoint& where, const std::string& method, const std::string& path, const header_list& headers,
                   std::string body, std::string content_type, request_spec& out, response& error) const {
        std::lock_guard<std::mutex> lock(mu_);
        return s_.make_spec(where, method, path, headers, std::move(body), std::move(content_type), out, error);
    }
    bool make_spec_for_url(const std::string& full_url, const std::string& extra_query, const std::string& method,
                           const header_list& headers, std::string body, std::string content_type, request_spec& out,
                           response& error) const {
        std::lock_guard<std::mutex> lock(mu_);
        return s_.make_spec_for_url(full_url, extra_query, method, headers, std::move(body), std::move(content_type), out, error);
    }

private:
    mutable std::mutex mu_;
    session_settings s_;
};

// "scheme://host[:port]" + the base path joined to `path`: the URL a request made from a session starts with. An IPv6 host is
// put in brackets.
std::string endpoint_url(const endpoint& where, const std::string& path);

// The engine a spec runs on: the shared one, or the download one when it streams to a sink.
async_http& engine_for(const request_spec& spec);

// The engine job for one hop of `spec`.
async_job make_job(const request_spec& spec);

// Resolves a Location header against the hop that produced it. False: not something to follow.
bool resolve_location(const request_spec& current, std::string location, request_spec& next);

// Runs `spec` on the shared engine and follows redirects if asked. `done` runs on the engine thread,
// once, with the final response.
void run_chain(std::shared_ptr<request_spec> spec, std::function<void(response)> done);

// Same, and waits. Called on the engine's own thread it would wait for itself, so it returns an
// error response (error_kind::other) instead.
response run_blocking(std::shared_ptr<request_spec> spec);

} // namespace httpp::detail
