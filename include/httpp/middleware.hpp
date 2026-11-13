#pragma once

#include "httpp/export.hpp"
#include "httpp/server.hpp"

#include <cstddef>
#include <string>
#include <vector>

// Ready-made middleware (ADR-0037, ADR-0066). Each class installs a pair of hooks with server::use(), so any
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
    HTTPP_API cors& set_allow_origins(std::vector<std::string> origins);
    // An ECMAScript regular expression the whole origin must match; "" = none. A bad expression throws.
    HTTPP_API cors& set_allow_origin_regex(std::string pattern);
    // "*" = all. Default: GET.
    HTTPP_API cors& set_allow_methods(std::vector<std::string> methods);
    // "*" = whatever the preflight asks for. Accept, Accept-Language, Content-Language and Content-Type are always allowed.
    HTTPP_API cors& set_allow_headers(std::vector<std::string> headers);
    // With "*" origins the request's own origin is echoed instead of "*". Default: false.
    HTTPP_API cors& set_allow_credentials(bool allow);
    HTTPP_API cors& set_expose_headers(std::vector<std::string> headers);
    // Seconds a browser may cache a preflight; negative throws. Default: 600.
    HTTPP_API cors& set_max_age(int seconds);
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
    HTTPP_API trusted_host& set_allowed_hosts(std::vector<std::string> hosts);
    HTTPP_API trusted_host& set_www_redirect(bool redirect);
    HTTPP_API void install(server& s) const;

private:
    std::vector<std::string> hosts_;
    bool www_redirect_ = true;
};

// Send requests that did not arrive over TLS to the https:// address of the same URL (307, keeping the Host header's port unless it is 80).
// The server itself serves TLS or plain HTTP on a port, never both, so this is for a plain-HTTP server behind a proxy that terminates TLS
// and says so with "X-Forwarded-Proto: https" (set_trust_forwarded_proto(true); leave it off when clients can reach this port directly, because
// they could then claim any scheme), or just to point people at the secure address. Requests on a TLS server are never redirected.
class https_redirect {
public:
    HTTPP_API https_redirect();
    HTTPP_API https_redirect& set_trust_forwarded_proto(bool trust);
    HTTPP_API void install(server& s) const;

private:
    bool trust_ = false;
};

} // namespace httpp
