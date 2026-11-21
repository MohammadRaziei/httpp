#pragma once

#include <optional>
#include <string>

#include "httpp/export.hpp"
#include "httpp/url.hpp"

namespace httpp {

// The proxy a program such as curl would use for `target`, read from the environment (ADR-0067). A request does not do this on
// its own: it goes direct unless you call request::proxy() or client::proxy(), so a library never sends traffic to a proxy by surprise. Give the
// result to proxy() when you want curl's behavior (`httpp curl` does).
//
//   http:// target   http_proxy               (lowercase only, as in curl: HTTP_PROXY is ignored, see "httpoxy")
//   https:// target  https_proxy, HTTPS_PROXY
//   either, when that one is unset or empty:  all_proxy, ALL_PROXY
//
// A value is a proxy URL (http://, socks5://, socks5h://) or "host[:port]", which means http://host[:port], as in curl. A value
// that is not usable throws std::invalid_argument naming it. An empty variable counts as unset.
//
// no_proxy / NO_PROXY (the lowercase one first) lists targets that go direct: names or addresses separated by commas or spaces.
// "*" is everything; a name matches itself and its subdomains (a leading "." or "*." is ignored, case does not matter, and
// "example.com" does not match "notexample.com"); an IPv4 address matches itself, and "a.b.c.d/n" is a range.
// ponytail: no IPv6 ranges and no "host:port" entries; an IPv6 address matches only the exact text.
//
// Returns std::nullopt for a direct connection.
HTTPP_API std::optional<url> proxy_from_environment(const url& target);

// The same rules with values you give: `proxy` replaces the proxy variables (curl -x; "" and direct:// in any case, with nothing after the scheme, mean no proxy at all, ADR-0106; a bare "direct" or "off" is a host, and in an environment variable direct:// is an error, not "no proxy") and `no_proxy`
// replaces NO_PROXY (curl --noproxy; "" excludes nothing). std::nullopt reads the environment for that part. The no_proxy list
// applies to an explicit proxy too.
HTTPP_API std::optional<url> resolve_proxy(const url& target, const std::optional<std::string>& proxy,
                                           const std::optional<std::string>& no_proxy);

} // namespace httpp
