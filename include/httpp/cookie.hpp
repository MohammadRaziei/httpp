// httpp::cookie: one cookie, for both directions. A server reads the request's `Cookie` header with cookie::parse_cookie_header and answers with
// `Set-Cookie` headers built from cookie objects; a client sends `Cookie` pairs. The rules (what a name and a value may contain, how an attribute
// is written) live here once, so C++ and Python (httpp.set_cookie, request.cookies) cannot drift apart.
//
//     res.set_cookie(httpp::cookie("sid", token).max_age(3600).secure(true).same_site_policy(httpp::same_site::lax));
//     for (const auto& [name, value] : httpp::cookie::parse_cookie_header(req.header("Cookie"))) ...
//
// A name is an RFC 6265 token; a value is made of cookie-octets (no space, quote, comma, semicolon, backslash or control character: percent-encode
// anything else yourself). The constructor and the setters throw std::invalid_argument for anything that could not be written safely into a
// header, so a cookie that exists is always one that can be sent; a failed setter leaves the cookie unchanged.
// Defaults match what httpp always sent: `Path=/; HttpOnly`.
#pragma once

#include "httpp/export.hpp"

#include <string>
#include <utility>
#include <vector>

namespace httpp {

enum class same_site { lax, strict, none };

class cookie {
public:
    HTTPP_API cookie(std::string name, std::string value);
    HTTPP_API cookie& path(std::string path);     // "" leaves the attribute out
    HTTPP_API cookie& domain(std::string domain); // "" (the default) leaves the attribute out
    HTTPP_API cookie& max_age(long long seconds); // 0 or less asks the client to delete the cookie
    HTTPP_API cookie& secure(bool on);
    HTTPP_API cookie& http_only(bool on);
    HTTPP_API cookie& same_site_policy(httpp::same_site site);

    const std::string& name() const noexcept { return name_; }
    const std::string& value() const noexcept { return value_; }

    // The value of a `Set-Cookie` header: "sid=abc; Path=/; Max-Age=3600; Secure; HttpOnly; SameSite=Lax".
    HTTPP_API std::string to_set_cookie() const;

    // The pairs of a request's `Cookie` header ("a=1; b=2"), in order, repeated names included (the first one is the one to believe). Spaces and tabs
    // around a pair are skipped; a part with no '=' or an empty name is ignored. Nothing is decoded and nothing throws.
    HTTPP_API static std::vector<std::pair<std::string, std::string>> parse_cookie_header(const std::string& header);

private:
    std::string name_, value_;
    std::string path_ = "/", domain_;
    long long max_age_ = 0;
    bool has_max_age_ = false, secure_ = false, http_only_ = true;
    bool has_same_site_ = false;
    httpp::same_site same_site_ = httpp::same_site::lax;
};

} // namespace httpp
