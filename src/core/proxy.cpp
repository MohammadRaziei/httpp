#include "httpp/proxy.hpp"

#include "internal/http_exchange.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace httpp {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// The first of these variables that is set and not empty.
std::string environment_value(std::initializer_list<const char*> names) {
    for (const char* name : names) {
        const char* value = std::getenv(name);
        if (value && *value) return value;
    }
    return std::string();
}

// "a.b.c.d" as a number; false for anything else (names, IPv6).
bool ipv4(std::string_view text, std::uint32_t& out) {
    std::uint32_t value = 0;
    int parts = 0;
    while (true) {
        std::size_t used = 0;
        unsigned part = 0;
        while (used < text.size() && std::isdigit(static_cast<unsigned char>(text[used])) && used < 3) part = part * 10 + (text[used++] - '0');
        if (used == 0 || part > 255) return false;
        value = (value << 8) | part;
        ++parts;
        text.remove_prefix(used);
        if (text.empty()) break;
        if (text.front() != '.') return false;
        text.remove_prefix(1);
    }
    out = value;
    return parts == 4;
}

// One no_proxy entry against the target's host (lowercase, no brackets).
bool entry_matches(std::string entry, const std::string& host) {
    if (entry.rfind("*.", 0) == 0) entry.erase(0, 2);
    while (!entry.empty() && entry.front() == '.') entry.erase(0, 1);
    if (entry.empty()) return false;

    std::uint32_t host_address = 0;
    const bool host_is_ipv4 = ipv4(host, host_address);
    const std::size_t slash = entry.find('/');
    if (slash != std::string::npos) { // a.b.c.d/n
        std::uint32_t base = 0;
        int bits = -1;
        try {
            std::size_t used = 0;
            bits = std::stoi(entry.substr(slash + 1), &used);
            if (used != entry.size() - slash - 1) bits = -1;
        } catch (const std::exception&) {
        }
        if (!host_is_ipv4 || !ipv4(std::string_view(entry).substr(0, slash), base) || bits < 0 || bits > 32) return false;
        const std::uint32_t mask = bits == 0 ? 0u : 0xFFFFFFFFu << (32 - bits);
        return (host_address & mask) == (base & mask);
    }
    if (host == entry) return true;
    std::uint32_t address = 0;
    if (ipv4(entry, address)) return false; // an address entry is an exact match only: "1.2.3.4" is not a domain suffix
    return host.size() > entry.size() && host.compare(host.size() - entry.size(), entry.size(), entry) == 0 &&
           host[host.size() - entry.size() - 1] == '.';
}

bool no_proxy_matches(const url& target, const std::string& list) {
    std::string host = lower(url_host(target));
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    std::size_t at = 0;
    while (at < list.size()) {
        std::size_t end = list.find_first_of(", \t\r\n", at);
        if (end == std::string::npos) end = list.size();
        const std::string_view entry = trimmed(std::string_view(list).substr(at, end - at));
        at = end + 1;
        if (entry.empty()) continue;
        if (entry == "*") return true;
        if (entry_matches(lower(std::string(entry)), host)) return true;
    }
    return false;
}

// The text of a proxy (a variable or -x) as a url httpp can use; "host[:port]" is http://host[:port], as in curl.
url proxy_url(const std::string& raw) {
    const std::string text(trimmed(raw));
    url u = parse_url(text.find("://") == std::string::npos ? "http://" + text : text);
    if (u.str().empty()) throw std::invalid_argument("proxy: '" + text + "' is not an http://, socks5:// or socks5h:// URL with a host");
    // Only an explicit value can mean "no proxy" (ADR-0106): from the environment it is an error, as any unknown scheme is, so that the
    // proxy found here is always a real one.
    if (detail::is_proxy_direct_url(u)) throw std::invalid_argument("proxy: '" + text + "' (direct://) turns the proxy off only as an explicit value, not in an environment variable");
    detail::parse_proxy_url(u); // throws, naming the text, for https:// and any other scheme, or for a SOCKS5 login that is too long
    return u;
}

} // namespace

std::optional<url> resolve_proxy(const url& target, const std::optional<std::string>& proxy, const std::optional<std::string>& no_proxy) {
    std::string text;
    if (proxy) {
        text = *proxy;
    } else if (target.protocol() == "https") {
        text = environment_value({"https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY"});
    } else {
        text = environment_value({"http_proxy", "all_proxy", "ALL_PROXY"}); // not HTTP_PROXY: see the header
    }
    // "" and, given explicitly, direct:// (ADR-0106) mean no proxy at all. In the environment direct:// is not accepted (see proxy_url).
    if (trimmed(text).empty() || (proxy && detail::is_proxy_direct_url(parse_url(trimmed(text))))) return std::nullopt;
    url found = proxy_url(text);
    if (no_proxy_matches(target, no_proxy ? *no_proxy : environment_value({"no_proxy", "NO_PROXY"}))) return std::nullopt;
    return found;
}

std::optional<url> proxy_from_environment(const url& target) { return resolve_proxy(target, std::nullopt, std::nullopt); }

} // namespace httpp
