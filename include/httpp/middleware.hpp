#pragma once

#include "httpp/export.hpp"
#include "httpp/server.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

// Ready-made middleware (ADR-0037 and 0066). Each class installs a pair of hooks with server::use(), so any
// number of them can be added, in any order, next to the single before/after hook of the application.
// The Python names are the same (Server.add_middleware(CORSMiddleware, ...)), with the same options as keyword arguments.
namespace httpp {

// Cross-Origin Resource Sharing, shaped like Starlette's CORSMiddleware.
//  - A preflight (OPTIONS with Origin and Access-Control-Request-Method) is answered here: 200 with the
//    Access-Control-* headers when origin, method and headers are allowed, else 400 with a plain text reason.
//  - Any other request that has an Origin header gets Access-Control-Allow-Origin (and Vary: Origin when the
//    answer depends on the origin) on the way out, whatever the status. A disallowed origin is still served,
//    just without the header, so the browser blocks it.
// Every setter validates its argument and throws std::invalid_argument, leaving the object unchanged; the
// object can be copied and installed on several servers.
class cors {
public:
    HTTPP_API cors();
    // Exact origins, or "*" for any.
    HTTPP_API cors& allow_origins(std::vector<std::string> origins);
    // An ECMAScript regular expression the whole origin must match; "" = none. A bad expression throws.
    HTTPP_API cors& allow_origin_regex(std::string pattern);
    // "*" = all. Default: GET.
    HTTPP_API cors& allow_methods(std::vector<std::string> methods);
    // "*" = whatever the preflight asks for. Accept, Accept-Language, Content-Language and Content-Type are always allowed.
    HTTPP_API cors& allow_headers(std::vector<std::string> headers);
    // With "*" origins the request's own origin is echoed instead of "*". Default: false.
    HTTPP_API cors& allow_credentials(bool allow);
    HTTPP_API cors& expose_headers(std::vector<std::string> headers);
    // Seconds a browser may cache a preflight; negative throws. Default: 600.
    HTTPP_API cors& max_age(int seconds);
    HTTPP_API void install(server& s) const;

private:
    std::vector<std::string> origins_;
    std::string origin_regex_;
    std::vector<std::string> methods_;
    std::vector<std::string> headers_;
    bool credentials_ = false;
    std::vector<std::string> expose_;
    int max_age_ = 600;
};

// Refuse requests whose Host header is not an allowed host (a 400 "Invalid host header"), against
// host-header attacks. The port is ignored. An entry is an exact name, "*" (anything), or "*.example.com"
// (any subdomain, not example.com itself); an empty list or an entry such as "*example.com" throws.
// With www_redirect (default on), a request for "example.com" is redirected (307) to "www.example.com"
// when only the latter is allowed (and the reverse).
class trusted_host {
public:
    HTTPP_API explicit trusted_host(std::vector<std::string> allowed_hosts);
    HTTPP_API trusted_host& allowed_hosts(std::vector<std::string> hosts);
    HTTPP_API trusted_host& www_redirect(bool redirect);
    HTTPP_API void install(server& s) const;

private:
    std::vector<std::string> hosts_;
    bool www_redirect_ = true;
};

// Send requests that did not arrive over TLS to the https:// address of the same URL (307, keeping the Host header's port unless it is 80).
// The server itself serves TLS or plain HTTP on a port, never both, so this is for a plain-HTTP server behind a proxy that terminates TLS
// and says so with "X-Forwarded-Proto: https" (trust_forwarded_proto(true); leave it off when clients can reach this port directly, because
// they could then claim any scheme), or just to point people at the secure address. Requests on a TLS server are never redirected.
class https_redirect {
public:
    HTTPP_API https_redirect();
    HTTPP_API https_redirect& trust_forwarded_proto(bool trust);
    HTTPP_API void install(server& s) const;

private:
    bool trust_ = false;
};

// Behind a reverse proxy (nginx, Caddy, a load balancer) every connection comes from the proxy. When the connection's address is one of
// the trusted proxies, this takes the real client from the headers the proxy adds, before any hook or handler runs (ADR-0069):
//  - X-Forwarded-For: server_request::client_ip becomes the first address from the right that is not itself a trusted proxy (the
//    leftmost one when all are), so a client cannot name itself by sending the header; a value that is not an address is ignored.
//  - X-Forwarded-Proto: "https" / "http" sets server_request::secure (and so scheme()), the last value in a list.
//  - X-Forwarded-Host: replaces the Host header, only with trust_forwarded_host(true), the last value in a list.
// A connection from anywhere else is left alone, whatever headers it sends. Forwarded (RFC 7239) is not read.
// A proxy entry is an IPv4 or IPv6 address, a CIDR range ("10.0.0.0/8", "fd00::/8"), or "*" (every connection: only when the port can
// be reached through the proxy alone). Default: 127.0.0.1 and ::1. An entry that is none of these throws std::invalid_argument and leaves
// the object unchanged; an empty list trusts nobody. One per server: installing a second replaces the first.
class forwarded_headers {
public:
    HTTPP_API forwarded_headers();
    HTTPP_API forwarded_headers& trusted_proxies(std::vector<std::string> proxies);
    HTTPP_API forwarded_headers& trust_forwarded_host(bool trust); // default false
    HTTPP_API void install(server& s) const;

private:
    std::vector<std::string> proxies_;
    bool host_ = false;
};

// HTTP Basic authentication (RFC 7617). A request whose Authorization header is missing, is not "Basic", does not decode or names an unknown
// user or a wrong password is answered 401 with `WWW-Authenticate: Basic realm="...", charset="UTF-8"`; the rest goes on. Passwords are compared
// in constant time against the configured list, which is kept in memory in clear text: this is a gate for a small tool or an internal service,
// not a user database. Credentials travel in clear text too, so use it on a TLS server (or behind a proxy that terminates TLS).
// A browser's preflight carries no credentials: install cors before this, so the preflight is answered first. `path_prefix` limits the check
// to the paths under a prefix ("/admin" covers "/admin" and "/admin/x", not "/administrator"); the default "/" covers everything.
// Users are entries "user:password" (the first ':' separates them, so a password may contain ':'; a user may not, and may not be empty); no
// users at all (and no verifier) throws at install(), since every request would be refused. Every setter validates its argument and throws
// std::invalid_argument, leaving the object unchanged.
// `verifier` takes over the password check: fn(user, password) returns true to let the request in. Use it to check against hashes (bcrypt,
// argon2: httpp does not hash passwords itself), a database or any other store. With users and a verifier, a request passes when either accepts
// it. fn runs on a server thread for every protected request, possibly from several threads at once, so it must be thread-safe and quick; it
// must compare secrets in constant time itself. If it throws, the exception goes to the server's error handling (a 500), the request is not let in.
class basic_auth {
public:
    HTTPP_API explicit basic_auth(std::string realm = "Restricted");
    // No '"', '\\' or control characters (the realm goes into a quoted header value).
    HTTPP_API basic_auth& realm(std::string realm);
    HTTPP_API basic_auth& users(std::vector<std::string> credentials);
    HTTPP_API basic_auth& path_prefix(std::string prefix);
    HTTPP_API basic_auth& verifier(std::function<bool(const std::string& user, const std::string& password)> fn);
    HTTPP_API void install(server& s) const;

private:
    std::string realm_;
    std::vector<std::string> users_;
    std::string prefix_ = "/";
    std::function<bool(const std::string&, const std::string&)> verifier_;
};

// A token bucket for every client address (server_request::client_ip, so put forwarded_headers first behind a proxy, or every request seems to come
// from the proxy): `rate` requests per second are earned, up to `burst` can be spent at once. A request that finds the bucket empty is answered
// 429 "Too Many Requests" with `Retry-After` (whole seconds, at least 1) and does not reach a handler. The table is bounded: at `max_clients`
// addresses the idle ones (full buckets) are dropped first, then an arbitrary one, so a flood of fresh addresses cannot grow memory without limit.
// The state lives in the installed hooks, so installing one object on two servers gives each its own table. Setters throw std::invalid_argument
// (rate not above 0 or not finite, burst or max_clients 0) and leave the object unchanged. Defaults: 10 requests per second, burst 20,
// 100000 clients.
class rate_limit {
public:
    HTTPP_API rate_limit();
    HTTPP_API rate_limit& rate(double requests_per_second);
    HTTPP_API rate_limit& burst(std::size_t burst);
    HTTPP_API rate_limit& max_clients(std::size_t max_clients);
    HTTPP_API void install(server& s) const;

private:
    double rate_ = 10;
    std::size_t burst_ = 20;
    std::size_t max_clients_ = 100000;
};

} // namespace httpp
