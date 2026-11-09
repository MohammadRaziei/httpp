#pragma once

// Internal, NOT installed. The client-side policy layer, shared by every public client class
// (httpp::client, httpp::client::request, httpp::async_client) and, later, by downloads (ADR-0051).
//
//   session_settings  what a caller configures once (headers, timeout, auth, proxy, TLS, redirects)
//   request_spec      one logical request, built from settings (Builder: session_settings::make_spec)
//   run_chain         runs a spec on the engine and follows redirects (the policy loop)
//   run_blocking      waits for run_chain (Adapter: sync on top of async)
//
// The engine (async_http.hpp) moves bytes; everything about *what* to send and which redirects to
// follow lives here, once. This header includes neither Asio nor cpp-httplib.

#include "httpp/client.hpp"
#include "async_http.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace httpp::detail {

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

    std::string method; // upper case
    std::string scheme, host;
    int port = 80;
    std::string target; // "/path?query", percent-encoded
    header_list headers;
    std::string body, content_type;
    long timeout = 0;
    bool follow = false;
    bool verify = true;
    std::string ca_file;
    std::string proxy_host;
    int proxy_port = 0;
    std::size_t max_response_size = 0;
    bool keep_alive = true; // keep the connection in the engine's pool for the next request
    int redirects_left = redirect_limit;
};

// Where a session sends its requests.
struct endpoint {
    std::string scheme = "http", host;
    int port = 80;
    std::string base_path; // prepended to every path, no trailing slash
};

// Everything a caller configures once and that applies to each request made afterwards.
struct session_settings {
    header_list headers;
    long timeout = 0; // 0 = default_timeout
    bool follow = false;
    bool verify = true;
    std::string ca_file;
    std::string proxy_host;
    int proxy_port = 0;
    std::size_t max_response_size = 0;
    bool keep_alive = true;
    bool has_basic = false;
    std::string user, password, bearer;

    // The session default, replacing a same-name one (the session setter).
    void set_default_header(std::string name, std::string value);
    void set_basic(std::string user_name, std::string pass);
    void set_bearer(std::string token);
    void set_timeout(long seconds) { if (seconds > 0) timeout = seconds; }

    // Builder: the spec for one request. `call_headers` replace same-name session headers. Credentials
    // from the settings are added unless a call header already carries Authorization. Returns false
    // (and `error` is set) for a method httpp does not support.
    bool make_spec(const endpoint& where, const std::string& method, const std::string& path,
                   const header_list& call_headers, std::string body, std::string content_type,
                   request_spec& out, response& error) const;
};

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
