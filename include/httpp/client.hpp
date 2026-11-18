#pragma once

#include "httpp/cookie.hpp"
#include "httpp/export.hpp"
#include "httpp/path.hpp"
#include "httpp/url.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
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
    //   res.file = httpp::path("public") / "index.html";
    path file;
    // If set, the body is produced piece by piece with chunked transfer encoding. It is called
    // repeatedly with the number of bytes sent so far: put the next piece in `chunk` and return
    // true, or return false when the body is complete (a non-empty `chunk` returned together with
    // false is still sent).
    std::function<bool(std::size_t offset, std::string& chunk)> stream;
    // If set, runs once on a worker thread after the answer has been written to the connection (or the
    // connection failed while writing it): the place for work the client should not wait for (an email, a log
    // line, a cache refresh). The client already has its answer, so nothing it throws can reach it: a throw is
    // logged and ignored. It does not run if the answer was never sent (the handler gave up, the server was
    // stopped first). Keep it short: it holds a worker thread, like any sync handler.
    std::function<void()> after_send;

    response& redirect(const std::string& location, int code = 302) {
        status = code;
        headers.emplace_back("Location", location);
        return *this;
    }
    // Adds a Set-Cookie header; call it again for more cookies. Throws std::invalid_argument for a name or value that cannot be sent (httpp/cookie.hpp).
    response& set_cookie(const httpp::cookie& c) {
        headers.emplace_back("Set-Cookie", c.to_set_cookie());
        return *this;
    }
    response& set_cookie(const std::string& name, const std::string& value) { return set_cookie(httpp::cookie(name, value)); } // Path=/; HttpOnly
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

// Public API exposes no third-party types. The implementation (src/core/client.cpp) is hidden
// behind a pointer and runs every call on the library's non-blocking network engine.
//
// A `client` is a session: it keeps its connection alive between calls (pooled in the engine), so
// repeated requests to one server skip the TCP/TLS handshake. It is safe to share between threads and
// calls run side by side; each call uses the settings in force when it started. A blocking call
// waits for the answer, so it must not be made from an async callback (the network thread): that
// returns error_kind::other at once instead of hanging.
// Network failures never throw: they come back as a response with status 0 and error set. (A malformed
// base URL passed to a constructor is a programming error and throws std::invalid_argument.)
class client {
public:
    using headers_t = std::vector<std::pair<std::string, std::string>>;

    // use_ssl selects https (mbedtls-backed) or plain http; fetch()/httpp::request take either
    // kind of URL without the caller ever thinking about this.
    explicit HTTPP_API client(const std::string& host, int port, bool use_ssl = false);
    // "http://host[:port][/base/path]" or "https://...". A base path is prepended to every
    // request path. Throws std::invalid_argument if the URL is not a usable http(s) URL.
    explicit HTTPP_API client(const url& base_url);
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
    // Send requests through an HTTP proxy: "http://[user[:pass]@]host[:port]" (user and pass percent-decoded, sent as Proxy-Authorization: Basic).
    // http:// targets go to the proxy in absolute form, https:// targets through a CONNECT tunnel. "" = no proxy. Anything that is not
    // an http:// URL with a host ("host:port", "https://...", "socks5://...") throws std::invalid_argument and changes nothing.
    HTTPP_API client& set_proxy(const url& proxy_url);
    HTTPP_API client& set_verify(bool enable); // https: verify the server certificate (default true)
    HTTPP_API client& set_ca_file(const path& file); // https: trust this CA bundle instead of the system's
    HTTPP_API client& set_keep_alive(bool enable); // default true

    // Parse `full_url` (via httpp::url) and GET it in one call - handles
    // http:// and https:// alike. No manual URL parsing needed by callers
    // (e.g. the CLI's `download` command).
    static HTTPP_API response fetch(const url& full_url);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// A fluent request builder for the less common cases (custom method,
// headers, a body, auth, cookies, a proxy) that get()/fetch() don't cover.
// It is also what the `httpp curl` command runs on.
//
// Named like Python's httpp.Request. The request a server handler receives is httpp::server_request (Python:
// httpp.ServerRequest), so the two names say which side they belong to: httpp::server_request(url)....run()
class request {
public:
    explicit HTTPP_API request(const url& target);
    HTTPP_API ~request();

    HTTPP_API request(request&&) noexcept;
    HTTPP_API request& operator=(request&&) noexcept;
    request(const request&) = delete;
    request& operator=(const request&) = delete;

    // -X METHOD (GET/HEAD/POST/PUT/PATCH/DELETE/OPTIONS). If never
    // called, defaults to GET, unless a body was set, in which case
    // it defaults to POST - matching curl's own behavior.
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
    // -L/--location and --max-redirs in one: how many redirects to follow. 0 (the default) follows none: a 3xx
    // is the answer. n > 0 follows up to n hops; one more fails with error_kind::too_many_redirects.
    // -1 follows without a limit (a redirect loop then never ends unless max_time() is set, as timeout() limits each
    // step, not the whole request). Throws std::invalid_argument for any other negative number.
    HTTPP_API request& max_redirects(int count);

    // On by default: ask for gzip / deflate (Accept-Encoding, unless header() sets one) and hand back the
    // decoded body. Off: no Accept-Encoding is sent unless header() sets one, and a coded body comes back
    // exactly as the server sent it, like requests with stream=True and raw bytes.
    HTTPP_API request& auto_compression(bool enable = true);
    // --connect-timeout (seconds): the limit for resolving the host, connecting, the proxy handshake and the
    // TLS handshake. timeout() still limits each write and read. 0 (the default) means timeout() covers the
    // connect steps too. Throws std::invalid_argument for a negative number.
    HTTPP_API request& connect_timeout(long seconds);
    // -m/--max-time (seconds): one limit for the whole request, from the start to the last body byte, redirects and a
    // streamed body included. When it runs out the request ends with error_kind::timeout, even if the server is still
    // sending. 0 (the default) sets no such limit. timeout() is separate: it limits each step, so a slow trickle never
    // trips it. Throws std::invalid_argument for a negative number.
    HTTPP_API request& max_time(long seconds);

    // https: verify the server certificate (default true), or trust this CA bundle instead of
    // the system's. And an HTTP proxy.
    HTTPP_API request& verify(bool enable);
    HTTPP_API request& ca_file(const path& file);
    HTTPP_API request& proxy(const url& proxy_url); // same URL form and rules as client::set_proxy

    // Upper bound, in bytes, on the response body this request will
    // buffer into response::body. 0 (the default) means no limit, which
    // matches curl/wget: run() hands you the whole body as a string, so
    // httpp does not second-guess how big that is. Set a non-zero value
    // to guard against a hostile or runaway server; exceeding it yields
    // response::status == 413 (Payload Too Large) with error too_large.
    HTTPP_API request& max_response_size(std::size_t bytes);

    // Streaming (ADR-0084): with on_body() the body of the final response goes to `piece` as it arrives, in
    // the order sent, instead of being collected: response::body stays empty and max_response_size does not
    // apply. Return false to stop the transfer (the response then has error_kind::canceled). The next piece
    // is read only after the callback returns, so a slow callback slows the download down, not the memory up.
    // The pieces are the bytes as the server sent them: the request asks for "Accept-Encoding: identity"
    // (an Accept-Encoding header of your own wins, and then a gzip body arrives still compressed).
    // The callbacks run on a network thread of their own, separate from the one plain requests use, so
    // a blocking write is fine but a long wait holds up other streaming requests; they must not call back
    // into a blocking httpp request. A redirect that is followed is never offered to them.
    HTTPP_API request& on_body(std::function<bool(std::string_view piece)> piece);
    // Optional, with on_body(): called once with the status and headers of the final response (body empty)
    // before the first piece. Return false to skip the body: it is read and thrown away, `piece` is not
    // called, and run() still returns the response (an error status you do not want written, say).
    HTTPP_API request& on_head(std::function<bool(const response& head)> head);

    HTTPP_API response run() const;
    // Starts the request on the library's non-blocking network thread and returns at once. No
    // thread is used per request, so thousands can be in flight. Connections are kept alive and
    // reused across requests to the same place; redirects (if enabled), TLS and the proxy all run
    // on that thread too.
    HTTPP_API std::future<response> run_async() const;
    // Same, but instead of a future `done` is called with the response, on the library's network
    // thread: it must be quick and must not block (hand real work to your own thread or loop).
    // A request that cannot even start (bad URL, unknown method) calls it at once, on this thread.
    HTTPP_API void run_async(std::function<void(response)> done) const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// Many concurrent requests to one server. Every call returns a future at once; the requests run on
// the library's non-blocking network thread over pooled keep-alive connections, at most
// `max_connections` in flight at a time (the rest wait their turn). Configure it before
// the first request.
class async_client {
public:
    using headers_t = client::headers_t;

    explicit HTTPP_API async_client(const url& base_url, std::size_t max_connections = 8);
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
    // Callback form of send(): see request::run_async(done) for the rules `done` must follow.
    HTTPP_API void send(const std::string& method, const std::string& path, const headers_t& headers,
                        const std::string& body, const std::string& content_type,
                        std::function<void(response)> done);

    HTTPP_API async_client& set_header(std::string name, std::string value);
    HTTPP_API async_client& set_timeout(long seconds);
    HTTPP_API async_client& set_follow_redirects(bool enable = true);
    HTTPP_API async_client& set_basic_auth(const std::string& user, const std::string& password);
    HTTPP_API async_client& set_bearer_token(const std::string& token);
    HTTPP_API async_client& set_verify(bool enable);
    HTTPP_API async_client& set_ca_file(const path& file);
    HTTPP_API async_client& set_proxy(const url& proxy_url); // same URL form and rules as client::set_proxy

private:
    struct impl;
    std::shared_ptr<impl> impl_;
};

} // namespace httpp
