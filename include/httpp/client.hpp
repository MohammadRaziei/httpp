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

    // Client side only, filled whether the request worked or failed (ADR-0097).
    // elapsed_ms: how long the whole request ran, redirects included, until the last byte or the failure. This is the clock
    // max_time() uses, so a timeout reports about max_time() * 1000.
    // bytes_received: body bytes of the final response that arrived, before any gzip / deflate decoding: what on_body() was
    // given, or what was collected into `body`. A request that timed out with 0 here never got an answer; one with more was
    // cut off while the server was still sending.
    long long elapsed_ms = 0;
    std::size_t bytes_received = 0;

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

class client;
class async_client;
class request;

namespace detail {
class guarded_settings; // src/core/internal/http_exchange.hpp
struct session_settings;

// What configurable<Self> calls: the one place where each setting and its rule is written (ADR-0094). It is a view of the
// settings of a client, an async_client or a request, not something to use directly.
class settings_ref {
public:
    explicit settings_ref(guarded_settings& settings) noexcept : g_(&settings) {}
    HTTPP_API void timeout(long seconds) const;
    HTTPP_API long timeout() const;
    HTTPP_API void connect_timeout(long seconds) const;
    HTTPP_API long connect_timeout() const;
    HTTPP_API void max_time(long seconds) const;
    HTTPP_API long max_time() const;
    HTTPP_API void max_redirects(int count) const;
    HTTPP_API int max_redirects() const;
    HTTPP_API void auto_compression(bool enable) const;
    HTTPP_API bool auto_compression() const;
    HTTPP_API void basic_auth(const std::string& user, const std::string& password) const;
    HTTPP_API void bearer(const std::string& token) const;
    HTTPP_API void proxy(const url& proxy_url) const;
    HTTPP_API void verify(bool enable) const;
    HTTPP_API bool verify() const;
    HTTPP_API void ca_file(const path& file) const;
    HTTPP_API path ca_file() const;

private:
    guarded_settings* g_;
};
} // namespace detail

// The settings that client, async_client and request share, under the same names, declared once (ADR-0094). Derive as
// `class client : public configurable<client>`; Self says what a setter returns, so a chain such as
// request(url).timeout(30).json(body) keeps its type.
//
// A call with an argument sets and returns *this (a value when the object is a temporary, so a chain on a temporary is not a
// dangling reference); the same name with no argument reads the value back (timeout(30) sets, timeout() returns it), ADR-0092.
// basic_auth, bearer and proxy have no getter: the first two are secrets, and the parsed proxy is not the URL that was given.
//
// On a client or async_client a setting applies to the requests made after the call; a request made from it with
// make_request(path) starts with a copy of them.
// (HTTPP_MEMBER expands to nothing; it marks the public names for tests/python/test_cpp_python_parity.py, which counts them for each
// of the three classes, see export.hpp.)
template <class Self>
class configurable {
public:
    // connect, read and write, in seconds; 0 or less: the default
    HTTPP_MEMBER Self& timeout(long seconds) & { ref().timeout(seconds); return self(); }
    Self timeout(long seconds) && { ref().timeout(seconds); return std::move(self()); }
    HTTPP_MEMBER long timeout() const { return ref().timeout(); }

    // --connect-timeout (seconds): the limit for resolving the host, connecting, the proxy handshake and the TLS handshake.
    // timeout() still limits each write and read. 0 (the default) means timeout() covers the connect steps too. Throws
    // std::invalid_argument for a negative number.
    HTTPP_MEMBER Self& connect_timeout(long seconds) & { ref().connect_timeout(seconds); return self(); }
    Self connect_timeout(long seconds) && { ref().connect_timeout(seconds); return std::move(self()); }
    HTTPP_MEMBER long connect_timeout() const { return ref().connect_timeout(); }

    // -m/--max-time (seconds): one limit for the whole request, from the start to the last body byte, redirects and a
    // streamed body included. When it runs out the request ends with error_kind::timeout, even if the server is still
    // sending. 0 (the default) sets no such limit. timeout() is separate: it limits each step, so a slow trickle never
    // trips it. Throws std::invalid_argument for a negative number.
    HTTPP_MEMBER Self& max_time(long seconds) & { ref().max_time(seconds); return self(); }
    Self max_time(long seconds) && { ref().max_time(seconds); return std::move(self()); }
    HTTPP_MEMBER long max_time() const { return ref().max_time(); }

    // -L/--location and --max-redirs in one: how many redirects to follow. 0 (the default) follows none: a 3xx is the
    // answer. n > 0 follows up to n hops; one more fails with error_kind::too_many_redirects. -1 follows without a limit
    // (a redirect loop then never ends unless max_time() is set, as timeout() limits each step, not the whole request).
    // Throws std::invalid_argument for any other negative number.
    HTTPP_MEMBER Self& max_redirects(int count) & { ref().max_redirects(count); return self(); }
    Self max_redirects(int count) && { ref().max_redirects(count); return std::move(self()); }
    HTTPP_MEMBER int max_redirects() const { return ref().max_redirects(); }

    // On by default: ask for gzip / deflate (Accept-Encoding, unless a header sets one) and hand back the decoded body.
    // Off: no Accept-Encoding is sent unless a header sets one, and a coded body comes back exactly as the server sent
    // it, like requests with stream=True and raw bytes.
    HTTPP_MEMBER Self& auto_compression(bool enable) & { ref().auto_compression(enable); return self(); }
    Self auto_compression(bool enable) && { ref().auto_compression(enable); return std::move(self()); }
    HTTPP_MEMBER bool auto_compression() const { return ref().auto_compression(); }

    HTTPP_MEMBER Self& basic_auth(const std::string& user, const std::string& password) & { ref().basic_auth(user, password); return self(); }
    Self basic_auth(const std::string& user, const std::string& password) && { ref().basic_auth(user, password); return std::move(self()); }
    HTTPP_MEMBER Self& bearer(const std::string& token) & { ref().bearer(token); return self(); }
    Self bearer(const std::string& token) && { ref().bearer(token); return std::move(self()); }

    // Send requests through an HTTP proxy: "http://[user[:pass]@]host[:port]" (user and pass percent-decoded, sent as
    // Proxy-Authorization: Basic). http:// targets go to the proxy in absolute form, https:// targets through a CONNECT
    // tunnel. "" = no proxy. Anything that is not an http:// URL with a host ("host:port", "https://...", "socks5://...")
    // throws std::invalid_argument and changes nothing.
    HTTPP_MEMBER Self& proxy(const url& proxy_url) & { ref().proxy(proxy_url); return self(); }
    Self proxy(const url& proxy_url) && { ref().proxy(proxy_url); return std::move(self()); }

    // https: verify the server certificate (default true), or trust this CA bundle instead of the system's ("" when none is set).
    HTTPP_MEMBER Self& verify(bool enable) & { ref().verify(enable); return self(); }
    Self verify(bool enable) && { ref().verify(enable); return std::move(self()); }
    HTTPP_MEMBER bool verify() const { return ref().verify(); }
    HTTPP_MEMBER Self& ca_file(const path& file) & { ref().ca_file(file); return self(); }
    Self ca_file(const path& file) && { ref().ca_file(file); return std::move(self()); }
    HTTPP_MEMBER path ca_file() const { return ref().ca_file(); }

protected:
    configurable() = default;
    configurable(const configurable&) = default;
    configurable& operator=(const configurable&) = default;
    ~configurable() = default;

private:
    Self& self() { return static_cast<Self&>(*this); }
    detail::settings_ref ref() const { return static_cast<const Self&>(*this).settings_(); }
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
//
// A `client` is a handle (ADR-0093), like `server` and `async_client`: a copy is the same session, with the same settings,
// so `auto admin = base; admin.bearer(token);` changes `base` too. clone() is the independent copy.
class client : public configurable<client> {
public:
    using headers_t = std::vector<std::pair<std::string, std::string>>;

    // use_ssl selects https (mbedtls-backed) or plain http; fetch()/httpp::request take either
    // kind of URL without the caller ever thinking about this.
    explicit HTTPP_API client(const std::string& host, int port, bool use_ssl = false);
    // "http://host[:port][/base/path]" or "https://...". A base path is prepended to every
    // request path. Throws std::invalid_argument if the URL is not a usable http(s) URL.
    explicit HTTPP_API client(const url& base_url);
    HTTPP_API ~client();

    HTTPP_API client(const client&) noexcept;
    HTTPP_API client& operator=(const client&) noexcept;
    HTTPP_API client(client&&) noexcept;
    HTTPP_API client& operator=(client&&) noexcept;

    // A new session to the same base URL with a copy of the current settings, independent of this one: later changes to either
    // do not reach the other. (A copy made with `=` or the copy constructor is the same session.)
    HTTPP_API client clone() const;

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

    // A request to `path` (joined to the base URL as get() does) that starts with the headers and the settings of this session
    // as they are now: a copy, so later changes to the session do not reach it, and it may outlive the session. Everything of
    // httpp::request is then available (method, json, params, cookies, streaming, ...): c.make_request("/users").method("POST").json(body).run().
    // httpp::request(url) is the same builder without a session.
    HTTPP_API httpp::request make_request(const std::string& path) const;

    // Session settings: those of configurable (timeout, connect_timeout, max_time, max_redirects, auto_compression, basic_auth,
    // bearer, proxy, verify, ca_file), and these two. They apply to the requests made after the call.
    HTTPP_API client& header(std::string name, std::string value); // sent with every request
    HTTPP_API client& keep_alive(bool enable); // default true
    HTTPP_API bool keep_alive() const;

    // Parse `full_url` (via httpp::url) and GET it in one call - handles
    // http:// and https:// alike. No manual URL parsing needed by callers
    // (e.g. the CLI's `download` command).
    static HTTPP_API response fetch(const url& full_url);

private:
    friend class configurable<client>;
    HTTPP_API detail::settings_ref settings_() const; // what configurable<client> calls; exported because that code is inline
    struct impl;
    std::shared_ptr<impl> impl_;
};

// A fluent request builder for the less common cases (custom method,
// headers, a body, auth, cookies, a proxy) that get()/fetch() don't cover.
// It is also what the `httpp curl` command runs on.
//
// Named like Python's httpp.Request. The request a server handler receives is httpp::server_request (Python:
// httpp.ServerRequest), so the two names say which side they belong to: httpp::server_request(url)....run()
//
// The shared settings (timeout, max_redirects, auth, proxy, TLS, ...) are those of configurable, see above.
// Every setter has two forms: on a named request it returns `request&`, on a temporary it returns the request by value, so
// `auto r = httpp::request(url).json(body).timeout(5);` works and `r` is a request (it is move-only).
class request : public configurable<request> {
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
    HTTPP_API request& method(std::string m) &;
    request method(std::string m) && { method(std::move(m)); return std::move(*this); }

    // -H "Name: value" (repeatable; on a session, header() replaces a default of the same name instead)
    HTTPP_API request& header(std::string name, std::string value) &;
    request header(std::string name, std::string value) && { header(std::move(name), std::move(value)); return std::move(*this); }

    // Query parameter, percent-encoded and appended to the URL's own query (repeatable).
    HTTPP_API request& param(std::string name, std::string value) &;
    request param(std::string name, std::string value) && { param(std::move(name), std::move(value)); return std::move(*this); }

    // -d "body" (sets Content-Type to
    // application/x-www-form-urlencoded unless content_type() overrides it)
    HTTPP_API request& data(std::string body) &;
    request data(std::string body) && { data(std::move(body)); return std::move(*this); }
    // A JSON document you serialized yourself: sets the body and Content-Type: application/json.
    HTTPP_API request& json(std::string document) &;
    request json(std::string document) && { json(std::move(document)); return std::move(*this); }
    // An urlencoded form body from these fields.
    HTTPP_API request& form(const std::vector<std::pair<std::string, std::string>>& fields) &;
    request form(const std::vector<std::pair<std::string, std::string>>& fields) && { form(fields); return std::move(*this); }

    HTTPP_API request& content_type(std::string type) &;
    request content_type(std::string type) && { content_type(std::move(type)); return std::move(*this); }

    HTTPP_API request& cookie(std::string name, std::string value) &; // repeatable
    request cookie(std::string name, std::string value) && { cookie(std::move(name), std::move(value)); return std::move(*this); }

    // Upper bound, in bytes, on the response body this request will
    // buffer into response::body. 0 (the default) means no limit, which
    // matches curl/wget: run() hands you the whole body as a string, so
    // httpp does not second-guess how big that is. Set a non-zero value
    // to guard against a hostile or runaway server; exceeding it yields
    // response::status == 413 (Payload Too Large) with error too_large.
    HTTPP_API request& max_response_size(std::size_t bytes) &;
    request max_response_size(std::size_t bytes) && { max_response_size(bytes); return std::move(*this); }

    // Streaming (ADR-0084): with on_body() the body of the final response goes to `piece` as it arrives, in
    // the order sent, instead of being collected: response::body stays empty and max_response_size does not
    // apply. Return false to stop the transfer (the response then has error_kind::canceled). The next piece
    // is read only after the callback returns, so a slow callback slows the download down, not the memory up.
    // The pieces are the bytes as the server sent them: the request asks for "Accept-Encoding: identity"
    // (an Accept-Encoding header of your own wins, and then a gzip body arrives still compressed).
    // The callbacks run on a network thread of their own, separate from the one plain requests use, so
    // a blocking write is fine but a long wait holds up other streaming requests; they must not call back
    // into a blocking httpp request. A redirect that is followed is never offered to them.
    HTTPP_API request& on_body(std::function<bool(std::string_view piece)> piece) &;
    request on_body(std::function<bool(std::string_view piece)> piece) && { on_body(std::move(piece)); return std::move(*this); }
    // Optional, with on_body(): called once with the status and headers of the final response (body empty)
    // before the first piece. Return false to skip the body: it is read and thrown away, `piece` is not
    // called, and run() still returns the response (an error status you do not want written, say).
    HTTPP_API request& on_head(std::function<bool(const response& head)> head) &;
    request on_head(std::function<bool(const response& head)> head) && { on_head(std::move(head)); return std::move(*this); }

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
    friend class configurable<request>;
    friend class client;
    friend class async_client;
    HTTPP_API detail::settings_ref settings_() const; // what configurable<request> calls; exported because that code is inline
    // A request that starts with a copy of a session's settings (client::make_request, async_client::make_request).
    request(const url& target, const detail::session_settings& from);
    struct impl;
    std::unique_ptr<impl> impl_;
};

// Many concurrent requests to one server. Every call returns a future at once; the requests run on
// the library's non-blocking network thread over pooled keep-alive connections, at most
// `max_connections` in flight at a time (the rest wait their turn). Configure it before
// the first request.
//
// Like `client`, an async_client is a handle (ADR-0093): a copy is the same session, clone() the independent one (the clone
// has the same base URL, settings and max_connections, and its own empty queue).
class async_client : public configurable<async_client> {
public:
    using headers_t = client::headers_t;

    explicit HTTPP_API async_client(const url& base_url, std::size_t max_connections = 8);
    HTTPP_API ~async_client();
    HTTPP_API async_client(const async_client&) noexcept;
    HTTPP_API async_client& operator=(const async_client&) noexcept;
    HTTPP_API async_client(async_client&&) noexcept;
    HTTPP_API async_client& operator=(async_client&&) noexcept;

    HTTPP_API async_client clone() const;

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

    // As client::make_request: a request to `path` with a copy of the settings. It runs on its own (run, run_async), not through
    // this session's max_connections.
    HTTPP_API httpp::request make_request(const std::string& path) const;

    // The same session settings as client, under the same names (those of configurable, and header).
    HTTPP_API async_client& header(std::string name, std::string value);

private:
    friend class configurable<async_client>;
    HTTPP_API detail::settings_ref settings_() const; // what configurable<async_client> calls; exported because that code is inline
    struct impl;
    std::shared_ptr<impl> impl_;
};

} // namespace httpp
