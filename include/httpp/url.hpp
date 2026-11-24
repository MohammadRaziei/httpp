#pragma once

#include "httpp/export.hpp"

#include <urlparser.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace httpp {

// httpp's URL type is liburlparser's (ADR-0067): one parser, one type, no wrapper. `httpp::url` and `urlparser::url` are the
// same class, so every function that needs a URL takes `const httpp::url&` and a string or a string literal converts to it
// implicitly (ADR-0067): `client::fetch("http://example.com/x")`, `httpp::request(text)`, `httpp::download("https://...")`.
//
// The parser is scheme-agnostic and lenient: "nonsense" parses to a url with an empty scheme and host. What httpp accepts is
// decided by the url_* functions below, so `url_valid("nonsense")` is false. One thing to know: constructing a url from text
// whose port is not a number or is out of range (`http://h:99999/`) throws std::invalid_argument, as liburlparser does;
// `parse_url` never throws and gives an empty url instead.
using url = urlparser::url;

// --- httpp's rules on top of the generic parser (free functions: the parser itself stays HTTP-agnostic) ---

// Parses without throwing: an unparsable text gives a default (empty) url, which url_valid() rejects.
inline url parse_url(std::string_view raw) noexcept {
    try {
        return url(raw);
    } catch (...) {
        return url();
    }
}

// The one place that decides what httpp accepts: a host, and a scheme httpp knows (http, https, and socks5 / socks5h for
// proxy addresses only).
inline bool url_valid(const url& u) {
    const std::string_view s = u.protocol();
    return (s == "http" || s == "https" || s == "socks5" || s == "socks5h") && !u.host_text().empty();
}
// url_valid() and http or https: what a request can be sent to. Everything that fetches checks this, not url_valid() alone.
inline bool url_is_http(const url& u) {
    const std::string_view s = u.protocol();
    return (s == "http" || s == "https") && !u.host_text().empty();
}
// The port of a URL is `u.effective_port()`: the one the URL names, else its scheme's default (http 80, https 443, socks5 and
// socks5h 1080 for a proxy address, ADR-0068), else 0.
inline std::string url_scheme(const url& u) { return std::string(u.protocol()); }
// The host as written, IPv6 literals keep their brackets ("[::1]").
inline std::string url_host(const url& u) { return std::string(u.host_text()); }
// The "user:pass@" part, percent-decoded; both empty when the URL has none. A URL with a user and no ":" has an empty password.
inline std::string url_username(const url& u) { return urlparser::percent_codec::decode(u.username()); }
inline std::string url_password(const url& u) { return urlparser::percent_codec::decode(u.password()); }
// The path with "." and ".." resolved; an empty path is "/".
inline std::string url_path(const url& u) { return u.abspath().empty() ? std::string("/") : u.abspath(); }
inline std::string url_query(const url& u) { return std::string(u.query()); }
// The last piece of the path: the file name `curl -O` and a browser give a download. "" when there is none (the path ends in
// "/"), when it is "." or "..", or when it holds a backslash. It is not percent-decoded.
inline std::string url_filename(const url& u) {
    const std::string p = url_path(u);
    const std::size_t slash = p.rfind('/');
    std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    if (name == "." || name == ".." || name.find('\\') != std::string::npos) return std::string();
    return name;
}

// --- percent-encoding (same names as Python's httpp.URL static methods) ---

// Percent-encode one URL component (RFC 3986: everything except letters, digits and "-._~"
// becomes %XX; a space becomes %20). `build_query` joins encoded pairs as "a=1&b=x%20y".
HTTPP_API std::string encode_component(const std::string& text);
HTTPP_API std::string build_query(const std::vector<std::pair<std::string, std::string>>& params);
HTTPP_API std::string decode_component(const std::string& text); // %XX -> byte; "+" stays "+"

// --- Public Suffix List ---

// httpp builds liburlparser without the Public Suffix List, so url.host() classification works but domain(), subdomain() and
// suffix() of a multi-part suffix such as "co.uk" are wrong until a list is loaded. This downloads one with httpp's own
// client, from `source` (default: the address liburlparser itself names, `urlparser::psl::instance().source_url()`), and
// loads it into `urlparser::psl::instance()`, replacing what was there. It blocks (up to `timeout_seconds`). Throws
// std::runtime_error when the download fails or the answer is not a 200 with a parsable list; the list that was loaded stays
// as it was. Call it once at start up: loading is not synchronized with parsing on other threads.
HTTPP_API void fetch_public_suffix_list(const std::string& source = std::string(), long timeout_seconds = 30);

} // namespace httpp
