#include "httpp/cookie.hpp"

#include "httpp/server.hpp"
#include "internal/basic_credentials.hpp"

#include <stdexcept>

namespace httpp {
namespace {

bool token_char(unsigned char c) {
    if (c >= '0' && c <= '9') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= 'a' && c <= 'z') return true;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

bool value_char(unsigned char c) { // RFC 6265 cookie-octet
    return c == 0x21 || (c >= 0x23 && c <= 0x2b) || (c >= 0x2d && c <= 0x3a) || (c >= 0x3c && c <= 0x5b) || (c >= 0x5d && c <= 0x7e);
}

void check_attribute(const std::string& what, const std::string& v) {
    for (unsigned char c : v)
        if (c == ';' || c < 0x20 || c == 0x7f) throw std::invalid_argument("cookie: invalid " + what);
}

std::string trimmed(const std::string& s, std::size_t a, std::size_t b) {
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

} // namespace

cookie::cookie(std::string name, std::string value) {
    if (name.empty()) throw std::invalid_argument("cookie: invalid name (empty)");
    for (unsigned char c : name) if (!token_char(c)) throw std::invalid_argument("cookie: invalid name");
    for (unsigned char c : value)
        if (!value_char(c)) throw std::invalid_argument("cookie: invalid value (percent-encode spaces, quotes, commas, semicolons)");
    name_ = std::move(name);
    value_ = std::move(value);
}

cookie& cookie::path(std::string path) { check_attribute("path", path); path_ = std::move(path); return *this; }
cookie& cookie::domain(std::string domain) { check_attribute("domain", domain); domain_ = std::move(domain); return *this; }
cookie& cookie::max_age(long long seconds) { max_age_ = seconds; has_max_age_ = true; return *this; }
cookie& cookie::secure(bool on) { secure_ = on; return *this; }
cookie& cookie::http_only(bool on) { http_only_ = on; return *this; }
cookie& cookie::same_site_policy(httpp::same_site site) { same_site_ = site; has_same_site_ = true; return *this; }

std::string cookie::to_set_cookie() const {
    std::string out = name_ + "=" + value_;
    if (!path_.empty()) out += "; Path=" + path_;
    if (!domain_.empty()) out += "; Domain=" + domain_;
    if (has_max_age_) out += "; Max-Age=" + std::to_string(max_age_);
    if (secure_) out += "; Secure";
    if (http_only_) out += "; HttpOnly";
    if (has_same_site_) out += same_site_ == httpp::same_site::lax ? "; SameSite=Lax" : same_site_ == httpp::same_site::strict ? "; SameSite=Strict" : "; SameSite=None";
    return out;
}

std::vector<std::pair<std::string, std::string>> cookie::parse_cookie_header(const std::string& header) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t pos = 0;
    while (pos <= header.size()) {
        std::size_t end = header.find(';', pos);
        if (end == std::string::npos) end = header.size();
        const std::size_t eq = header.find('=', pos);
        if (eq != std::string::npos && eq < end) {
            std::string name = trimmed(header, pos, eq);
            if (!name.empty()) out.emplace_back(std::move(name), trimmed(header, eq + 1, end));
        }
        pos = end + 1;
    }
    return out;
}

// What a request reads from its own headers (declared in server.hpp, kept here next to the rules they share).

std::vector<std::pair<std::string, std::string>> server_request::cookies() const { return cookie::parse_cookie_header(header("Cookie")); }

std::string server_request::cookie(const std::string& name) const {
    for (auto& [k, v] : cookies())
        if (k == name) return std::move(v);
    return "";
}

std::optional<std::pair<std::string, std::string>> server_request::basic_auth() const {
    std::string user, password;
    if (!detail::parse_basic_authorization(header("Authorization"), user, password)) return std::nullopt;
    return std::make_pair(std::move(user), std::move(password));
}

} // namespace httpp
