#pragma once

#include "httpp/export.hpp"
#include "httpp/server.hpp"

#include <cstddef>
#include <string>
#include <vector>

// Ready-made middleware (ADR-0037). Each function installs a pair of hooks with server::use(), so any
// number of them can be added, in any order, next to the single before/after hook of the application.
// The Python names are the same (Server.add_middleware(CORSMiddleware, ...)), with the same options.
namespace httpp {

// Cross-Origin Resource Sharing, shaped like Starlette's CORSMiddleware.
//  - A preflight (OPTIONS with Origin and Access-Control-Request-Method) is answered here: 200 with the
//    Access-Control-* headers when origin, method and headers are allowed, else 400 with a plain text reason.
//  - Any other request that has an Origin header gets Access-Control-Allow-Origin (and Vary: Origin when the
//    answer depends on the origin) on the way out, whatever the status. A disallowed origin is still served,
//    just without the header, so the browser blocks it.
struct cors_options {
    std::vector<std::string> allow_origins;   // exact origins, or "*" for any
    std::string allow_origin_regex;           // an ECMAScript regular expression the whole origin must match; "" = none
    std::vector<std::string> allow_methods = {"GET"};   // "*" = all
    std::vector<std::string> allow_headers;   // "*" = whatever the preflight asks for; Accept, Accept-Language,
                                              // Content-Language and Content-Type are always allowed
    bool allow_credentials = false;           // with "*" origins the request's own origin is echoed instead of "*"
    std::vector<std::string> expose_headers;
    int max_age = 600;                        // seconds a browser may cache a preflight
};
// Throws std::invalid_argument for a bad regex or a negative max_age.
HTTPP_API void add_cors(server& s, const cors_options& options);

// Refuse requests whose Host header is not one of `allowed_hosts` (a 400 "Invalid host header"), against
// host-header attacks. The port is ignored. An entry is an exact name, "*" (anything), or "*.example.com"
// (any subdomain, not example.com itself). With `www_redirect`, a request for "example.com" is redirected
// (307) to "www.example.com" when only the latter is allowed (and the reverse).
struct trusted_host_options {
    std::vector<std::string> allowed_hosts;
    bool www_redirect = true;
};
// Throws std::invalid_argument for an empty list or an entry such as "*example.com" or "a.*.com".
HTTPP_API void add_trusted_host(server& s, const trusted_host_options& options);

} // namespace httpp
