#pragma once

#include "httpp/export.hpp"

#include <string>
#include <utility>
#include <vector>

namespace httpp {

// httpp's own thin, http(s)-specific view of a URL: valid()/scheme()/host()/
// username()/password()/port()/path()/query(). The actual parsing (IPv4/IPv6/PSL-aware, RFC-ish)
// is done by the vendored liburlparser (src/third_party/liburlparser, a git
// submodule built from source - no system package). That dependency is
// used only in src/core/url.cpp and never appears in this public header.
class url {
public:
    // An invalid or unsupported text gives a url whose valid() is false (nothing throws); str() keeps the text as given. The two
    // converting constructors are deliberately implicit (ADR-0067): every function that needs a URL takes an httpp::url, so a string or a
    // string literal is parsed once at the call, and a url that was parsed already is passed as is.
    HTTPP_API url();
    HTTPP_API url(const std::string& raw);
    HTTPP_API url(const char* raw);
    static HTTPP_API url parse(const std::string& raw);

    // Percent-encode one URL component (RFC 3986: everything except letters, digits and "-._~"
    // becomes %XX; a space becomes %20). `build_query` joins encoded pairs as "a=1&b=x%20y".
    static HTTPP_API std::string encode_component(const std::string& text);
    static HTTPP_API std::string build_query(const std::vector<std::pair<std::string, std::string>>& params);
    static HTTPP_API std::string decode_component(const std::string& text); // %XX -> byte; "+" stays "+"

    // True when the text parsed and its scheme is one httpp knows: http, https, or socks5 / socks5h (proxy addresses only).
    bool valid() const { return valid_; }
    // valid() and http or https: what a request can be sent to. Everything that fetches checks this, not valid() alone.
    bool is_http() const { return valid_ && (scheme_ == "http" || scheme_ == "https"); }
    const std::string& str() const { return raw_; } // the text this url was made from, unchanged
    const std::string& scheme() const { return scheme_; }
    const std::string& host() const { return host_; }
    // The "user:pass@" part, percent-decoded; both empty when the URL has none. A URL with a user and no ":" has an empty password.
    const std::string& username() const { return username_; }
    const std::string& password() const { return password_; }
    int port() const { return port_; }
    const std::string& path() const { return path_; }
    const std::string& query() const { return query_; }

private:
    bool valid_ = false;
    std::string raw_;
    std::string scheme_;
    std::string host_;
    std::string username_;
    std::string password_;
    int port_ = 0;
    std::string path_;
    std::string query_;
};

} // namespace httpp
