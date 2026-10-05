#pragma once

#include "httpp/export.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace httpp {

// Why a request produced no usable HTTP response (see response::error).
enum class error_kind {
    none = 0,
    invalid_url,
    unsupported_method,
    connection,         // could not connect, or the connection dropped
    timeout,
    tls,                // handshake or certificate verification failed
    too_many_redirects,
    too_large,          // response body exceeded max_response_size (status is 413)
    canceled,
    other,
};

struct response {
    int status = 0;
    std::string body; // raw bytes: binary-safe
    std::vector<std::pair<std::string, std::string>> headers;

    // Client side only. `error` is none whenever an HTTP response was received, whatever its
    // status (a 404 is a response, not an error). Otherwise status is 0 (413 for too_large)
    // and `error` / `error_message` say why.
    error_kind error = error_kind::none;
    std::string error_message;

    // --- Server side only (what a route handler fills in) ---
    // If set, the body is streamed from this file, with a known Content-Length. Set Content-Type
    // yourself if it matters. The path is trusted: never build it from user input.
    std::string file;
    // If set, the body is produced piece by piece with chunked transfer encoding. It is called
    // repeatedly with the number of bytes sent so far: put the next piece in `chunk` and return
    // true, or return false when the body is complete (a non-empty `chunk` returned together with
    // false is still sent).
    std::function<bool(std::size_t offset, std::string& chunk)> stream;

    response& redirect(const std::string& location, int code = 302) {
        status = code;
        headers.emplace_back("Location", location);
        return *this;
    }
    // Adds a Set-Cookie header ("name=value; <attributes>"); call it again for more cookies.
    response& set_cookie(const std::string& name, const std::string& value,
                         const std::string& attributes = "Path=/; HttpOnly") {
        headers.emplace_back("Set-Cookie", name + "=" + value + (attributes.empty() ? "" : "; " + attributes));
        return *this;
    }
    // A JSON document you serialized yourself: sets the body and Content-Type: application/json.
    response& json(std::string document) {
        body = std::move(document);
        headers.emplace_back("Content-Type", "application/json");
        return *this;
    }

    bool ok() const { return status >= 200 && status < 300; }
    bool failed() const { return error != error_kind::none; }

    const char* error_name() const {
        switch (error) {
            case error_kind::none: return "none";
            case error_kind::invalid_url: return "invalid_url";
            case error_kind::unsupported_method: return "unsupported_method";
            case error_kind::connection: return "connection";
            case error_kind::timeout: return "timeout";
            case error_kind::tls: return "tls";
            case error_kind::too_many_redirects: return "too_many_redirects";
            case error_kind::too_large: return "too_large";
            case error_kind::canceled: return "canceled";
            case error_kind::other: return "other";
        }
        return "other";
    }

    // HTTP header names are case-insensitive (RFC 7230); this looks up
    // the first matching header and returns "" if it isn't present.
    std::string header(const std::string& name) const {
        auto ci_equal = [](const std::string& a, const std::string& b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
                       return std::tolower(x) == std::tolower(y);
                   });
        };
        for (const auto& [key, value] : headers) {
            if (ci_equal(key, name)) return value;
        }
        return "";
    }
};

// Public API exposes no third-party types (no httplib.h here at all).
// The real implementation (backed by vendored cpp-httplib + mbedtls for
// HTTPS) lives in src/core/client.cpp and is hidden behind this pointer, so
// neither httplib.h nor mbedtls ever leak into consumers of this header.
//
// A `client` is a session: it keeps its connection alive between calls, so repeated requests to
// one server skip the TCP/TLS handshake. Calls on one client are serialized (it is safe to share
// between threads, but they take turns); use several clients, or async_client, for parallelism.
// Network failures never throw: they come back as a response with status 0 and error set. (A malformed
// base URL passed to a constructor is a programming error and throws std::invalid_argument.)
class client {
public:
    using headers_t = std::vector<std::pair<std::string, std::string>>;

    // use_ssl selects httplib's SSLClient (mbedtls-backed) vs. its plain
    // Client internally; both http:// and https:// URLs work through
    // fetch()/client::request without the caller ever thinking about this.
    explicit HTTPP_API client(const std::string& host, int port, bool use_ssl = false);
    // "http://host[:port][/base/path]" or "https://...". A base path is prepended to every
    // request path. Throws std::invalid_argument if the URL is not a usable http(s) URL.
    explicit HTTPP_API client(const std::string& base_url);
    HTTPP_API ~client();

    HTTPP_API client(client&&) noexcept;
    HTTPP_API client& operator=(client&&) noexcept;
    client(const client&) = delete;
    client& operator=(const client&) = delete;

    HTTPP_API response get(const std::string& path);
    HTTPP_API response get(const std::string& path, const headers_t& headers);
    HTTPP_API response head(const std::string& path, const headers_t& headers = {});
    HTTPP_API response options(const std::string& path, const headers_t& headers = {});
    // Body methods. `content_type` defaults to application/x-www-form-urlencoded (like curl -d).
    HTTPP_API response post(const std::string& path, const std::string& body,
                            const std::string& content_type = "", const headers_t& headers = {});
    HTTPP_API response put(const std::string& path, const std::string& body,
                           const std::string& content_type = "", const headers_t& headers = {});
    HTTPP_API response patch(const std::string& path, const std::string& body,
                             const std::string& content_type = "", const headers_t& headers = {});
    HTTPP_API response del(const std::string& path, const std::string& body = "",
                           const std::string& content_type = "", const headers_t& headers = {});
    // The general form of all of the above.
    HTTPP_API response send(const std::string& method, const std::string& path, const headers_t& headers = {},
                            const std::string& body = "", const std::string& content_type = "");

    // Session settings; they apply to the requests made after the call.
    HTTPP_API client& set_header(std::string name, std::string value); // sent with every request
    HTTPP_API client& set_timeout(long seconds);                       // connect, read and write
    HTTPP_API client& set_follow_redirects(bool enable = true);
    HTTPP_API client& set_basic_auth(const std::string& user, const std::string& password);
    HTTPP_API client& set_bearer_token(const std::string& token);
    HTTPP_API client& set_proxy(const std::string& host, int port);
    HTTPP_API client& set_verify(bool enable); // https: verify the server certificate (default true)
    HTTPP_API client& set_ca_file(const std::string& path); // https: trust this CA bundle instead of the system's
    HTTPP_API client& set_keep_alive(bool enable); // default true

    // Parse `full_url` (via httpp::url) and GET it in one call — handles
    // http:// and https:// alike. No manual URL parsing needed by callers
    // (e.g. the CLI's `download` command).
    static HTTPP_API response fetch(const std::string& full_url);

    // A small, curl-flavored fluent request builder for the less common
    // cases (custom method, headers, a body) that get()/fetch() don't
    // cover. Also what httpp/curl_compat.h's `curl_easy_*` C API is built on
    // (see src/core/curl_compat.cpp) — one implementation of "run an HTTP
    // request" in httpp, not two.
    //
    // Nested under `client` (not a free `httpp::request`) because
    // `httpp::request` is already the incoming-request struct passed to
    // server route handlers (see httpp/server.hpp) — naming this
    // `client::request` avoids that collision and reads naturally next to
    // the class it's built on: httpp::client::request(url)....run()
    class request {
    public:
        explicit HTTPP_API request(std::string url);
        HTTPP_API ~request();

        HTTPP_API request(request&&) noexcept;
        HTTPP_API request& operator=(request&&) noexcept;
        request(const request&) = delete;
        request& operator=(const request&) = delete;

        // -X METHOD (GET/HEAD/POST/PUT/PATCH/DELETE/OPTIONS). If never
        // called, defaults to GET, unless a body was set, in which case
        // it defaults to POST — matching curl's own behavior.
        HTTPP_API request& method(std::string m);

        // -H "Name: value"
        HTTPP_API request& header(std::string name, std::string value);

        // Query parameter, percent-encoded and appended to the URL's own query (repeatable).
        HTTPP_API request& param(std::string name, std::string value);

        // -d "body" (sets Content-Type to
        // application/x-www-form-urlencoded unless content_type() overrides it)
        HTTPP_API request& data(std::string body);
        // A JSON document you serialized yourself: sets the body and Content-Type: application/json.
        HTTPP_API request& json(std::string document);
        // An urlencoded form body from these fields.
        HTTPP_API request& form(const std::vector<std::pair<std::string, std::string>>& fields);

        HTTPP_API request& content_type(std::string type);

        HTTPP_API request& basic_auth(const std::string& user, const std::string& password);
        HTTPP_API request& bearer(const std::string& token);
        HTTPP_API request& cookie(std::string name, std::string value); // repeatable

        // -m/--max-time (seconds), -L/--location
        HTTPP_API request& timeout(long seconds);
        HTTPP_API request& follow_redirects(bool enable = true);

        // https: verify the server certificate (default true), or trust this CA bundle instead of
        // the system's. And an HTTP proxy.
        HTTPP_API request& verify(bool enable);
        HTTPP_API request& ca_file(std::string path);
        HTTPP_API request& proxy(std::string host, int port);

        // Upper bound, in bytes, on the response body this request will
        // buffer into response::body. 0 (the default) means no limit, which
        // matches curl/wget: run() hands you the whole body as a string, so
        // httpp does not second-guess how big that is. Set a non-zero value
        // to guard against a hostile or runaway server; exceeding it yields
        // response::status == 413 (Payload Too Large) with error too_large.
        HTTPP_API request& max_response_size(std::size_t bytes);

        HTTPP_API response run() const;
        // Runs the request on a small shared worker pool and returns at once. The pool is bounded,
        // so this scales to tens of concurrent requests; it is not non-blocking socket I/O.
        HTTPP_API std::future<response> run_async() const;

    private:
        struct impl;
        static response run_impl(const impl& r);
        std::unique_ptr<impl> impl_;
    };

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// A pool of keep-alive connections to one server, for many concurrent requests. Every call
// returns a future at once; the requests run on the shared worker pool, each on its own pooled
// connection (up to `max_connections` at a time, the rest wait their turn). Configure it before
// the first request.
class async_client {
public:
    using headers_t = client::headers_t;

    explicit HTTPP_API async_client(const std::string& base_url, std::size_t max_connections = 8);
    HTTPP_API ~async_client();
    HTTPP_API async_client(async_client&&) noexcept;
    HTTPP_API async_client& operator=(async_client&&) noexcept;
    async_client(const async_client&) = delete;
    async_client& operator=(const async_client&) = delete;

    HTTPP_API std::future<response> get(const std::string& path, const headers_t& headers = {});
    HTTPP_API std::future<response> head(const std::string& path, const headers_t& headers = {});
    HTTPP_API std::future<response> post(const std::string& path, const std::string& body,
                                         const std::string& content_type = "", const headers_t& headers = {});
    HTTPP_API std::future<response> put(const std::string& path, const std::string& body,
                                        const std::string& content_type = "", const headers_t& headers = {});
    HTTPP_API std::future<response> patch(const std::string& path, const std::string& body,
                                          const std::string& content_type = "", const headers_t& headers = {});
    HTTPP_API std::future<response> del(const std::string& path, const std::string& body = "",
                                        const std::string& content_type = "", const headers_t& headers = {});
    HTTPP_API std::future<response> send(const std::string& method, const std::string& path,
                                         const headers_t& headers = {}, const std::string& body = "",
                                         const std::string& content_type = "");

    HTTPP_API async_client& set_header(std::string name, std::string value);
    HTTPP_API async_client& set_timeout(long seconds);
    HTTPP_API async_client& set_follow_redirects(bool enable = true);
    HTTPP_API async_client& set_basic_auth(const std::string& user, const std::string& password);
    HTTPP_API async_client& set_bearer_token(const std::string& token);
    HTTPP_API async_client& set_verify(bool enable);
    HTTPP_API async_client& set_ca_file(const std::string& path);

private:
    struct impl;
    std::shared_ptr<impl> impl_;
};

} // namespace httpp
