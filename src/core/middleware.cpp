#include "httpp/middleware.hpp"

#include <asio/ip/address.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace httpp {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

std::string join(const std::vector<std::string>& v, const char* sep = ", ") {
    std::string out;
    for (const auto& s : v) { if (!out.empty()) out += sep; out += s; }
    return out;
}

bool has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// ---------------------------------------------------------------- CORS

struct cors_policy {
    std::set<std::string> origins;
    std::unique_ptr<std::regex> origin_regex;
    bool any_origin = false;
    bool any_method = false;
    bool any_header = false;
    bool credentials = false;
    std::string allow_methods;   // header value
    std::string allow_headers;   // header value (configured spelling), without the safelisted four unless "*"
    std::set<std::string> header_set; // lower case, including the safelisted four
    std::string expose;
    std::string max_age;
    std::vector<std::string> methods;

    bool origin_ok(const std::string& o) const {
        if (any_origin || origins.count(o)) return true;
        return origin_regex && std::regex_match(o, *origin_regex);
    }
    // What goes in Access-Control-Allow-Origin for an allowed origin.
    std::string origin_value(const std::string& o) const { return (any_origin && !credentials) ? "*" : o; }
    bool vary_origin() const { return !(any_origin && !credentials); }
};

bool is_preflight(const server_request& r) {
    return r.method == "OPTIONS" && !r.header("Origin").empty() && !r.header("Access-Control-Request-Method").empty();
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

void bad_request(response& res, const std::string& why) {
    res.status = 400;
    res.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
    res.body = why;
}

// ---------------------------------------------------------------- trusted host

std::string host_name(const std::string& host_header) {
    std::string h = lower(trim(host_header));
    if (!h.empty() && h[0] == '[') { // "[::1]:8000"
        const auto end = h.find(']');
        return end == std::string::npos ? h : h.substr(0, end + 1);
    }
    const auto colon = h.rfind(':');
    if (colon != std::string::npos && h.find(':') == colon) h.resize(colon); // one colon: host:port
    return h;
}

struct host_policy {
    std::vector<std::string> exact;
    std::vector<std::string> suffixes; // ".example.com" for "*.example.com"
    bool any = false;
    bool www_redirect = true;

    bool allowed(const std::string& host) const {
        if (any) return true;
        if (has(exact, host)) return true;
        for (const auto& suf : suffixes)
            if (host.size() > suf.size() && host.compare(host.size() - suf.size(), suf.size(), suf) == 0) return true;
        return false;
    }
};

// ponytail: percent-encodes the path and query again for a redirect; the request keeps them decoded and the
// server has no raw target to hand out. Upgrade path: expose the raw target on `request`.
std::string encode(const std::string& s, bool keep_slash) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || (keep_slash && c == '/')) out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

// ---------------------------------------------------------------- forwarded headers

// An address as 16 bytes: IPv4 is kept as an IPv4-mapped IPv6 one, so one comparison covers both families.
using ip16 = std::array<unsigned char, 16>;

bool to_ip16(const std::string& text, ip16& out) {
    std::error_code ec;
    const auto a = asio::ip::make_address(text, ec);
    if (ec) return false;
    out = a.is_v4() ? asio::ip::make_address_v6(asio::ip::v4_mapped, a.to_v4()).to_bytes() : a.to_v6().to_bytes();
    return true;
}

struct network {
    ip16 base{};
    int bits = 128; // prefix length in the 16-byte form
    bool any = false;
    bool contains(const ip16& a) const {
        if (any) return true;
        for (int i = 0; i < 16 && bits > 0; ++i) {
            const int take = std::min(8, bits - i * 8);
            if (take <= 0) break;
            const unsigned char mask = static_cast<unsigned char>(0xFF << (8 - take));
            if ((a[i] & mask) != (base[i] & mask)) return false;
        }
        return true;
    }
};

bool parse_network(const std::string& raw, network& out) {
    const std::string s = trim(raw);
    if (s == "*") { out = network(); out.any = true; return true; }
    const auto slash = s.find('/');
    const std::string addr = s.substr(0, slash);
    ip16 b;
    if (!to_ip16(addr, b)) return false;
    std::error_code ec;
    const bool v4 = asio::ip::make_address(addr, ec).is_v4();
    int max_bits = v4 ? 32 : 128;
    int bits = max_bits;
    if (slash != std::string::npos) {
        const std::string len = s.substr(slash + 1);
        if (len.empty() || len.size() > 3 || !std::all_of(len.begin(), len.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return false;
        bits = std::stoi(len);
        if (bits > max_bits) return false;
    }
    out = network();
    out.base = b;
    out.bits = v4 ? bits + 96 : bits;
    return true;
}

struct forwarded_policy {
    std::vector<network> proxies;
    bool trust_host = false;
    bool trusted(const std::string& ip) const {
        ip16 a;
        if (!to_ip16(ip, a)) return false;
        for (const auto& n : proxies) if (n.contains(a)) return true;
        return false;
    }
};

std::vector<std::string> split_list(const std::string& v) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= v.size()) {
        std::size_t end = v.find(',', pos);
        if (end == std::string::npos) end = v.size();
        out.push_back(trim(v.substr(pos, end - pos)));
        pos = end + 1;
    }
    return out;
}

} // namespace

forwarded_headers::forwarded_headers() : proxies_{"127.0.0.1", "::1"} {}

forwarded_headers& forwarded_headers::set_trusted_proxies(std::vector<std::string> proxies) {
    for (const auto& p : proxies) {
        network n;
        if (!parse_network(p, n))
            throw std::invalid_argument("forwarded_headers: '" + p + "' is not an IP address, a CIDR range or \"*\"");
    }
    proxies_ = std::move(proxies);
    return *this;
}

forwarded_headers& forwarded_headers::set_trust_forwarded_host(bool trust) { host_ = trust; return *this; }

void forwarded_headers::install(server& s) const {
    auto p = std::make_shared<forwarded_policy>();
    p->trust_host = host_;
    for (const auto& raw : proxies_) { network n; parse_network(raw, n); p->proxies.push_back(n); }
    s.set_request_rewrite([p](server_request& req) {
        if (!p->trusted(req.client_ip)) return;
        const std::string xff = req.header("X-Forwarded-For");
        if (!xff.empty()) {
            const auto list = split_list(xff);
            std::string chosen = list.front();
            for (auto it = list.rbegin(); it != list.rend(); ++it) {
                chosen = *it;
                if (!p->trusted(*it)) break;
            }
            ip16 check;
            if (to_ip16(chosen, check)) req.client_ip = chosen;
        }
        const std::string proto = req.header("X-Forwarded-Proto");
        if (!proto.empty()) {
            const std::string last = lower(split_list(proto).back());
            if (last == "https") req.secure = true;
            else if (last == "http") req.secure = false;
        }
        if (p->trust_host) {
            const std::string host = req.header("X-Forwarded-Host");
            if (!host.empty()) {
                const std::string last = split_list(host).back();
                bool replaced = false;
                for (auto& [k, v] : req.headers)
                    if (lower(k) == "host") { v = last; replaced = true; break; }
                if (!replaced) req.headers.emplace_back("Host", last);
            }
        }
    });
}

cors::cors() : methods_{"GET"} {}

cors& cors::set_allow_origins(std::vector<std::string> origins) { origins_ = std::move(origins); return *this; }

cors& cors::set_allow_origin_regex(std::string pattern) {
    if (!pattern.empty()) {
        try {
            std::regex check(pattern, std::regex::ECMAScript);
        } catch (const std::regex_error& e) {
            throw std::invalid_argument(std::string("cors: bad allow_origin_regex: ") + e.what());
        }
    }
    origin_regex_ = std::move(pattern);
    return *this;
}

cors& cors::set_allow_methods(std::vector<std::string> methods) { methods_ = std::move(methods); return *this; }
cors& cors::set_allow_headers(std::vector<std::string> headers) { headers_ = std::move(headers); return *this; }
cors& cors::set_allow_credentials(bool allow) { credentials_ = allow; return *this; }
cors& cors::set_expose_headers(std::vector<std::string> headers) { expose_ = std::move(headers); return *this; }

cors& cors::set_max_age(int seconds) {
    if (seconds < 0) throw std::invalid_argument("cors: max_age must not be negative");
    max_age_ = seconds;
    return *this;
}

void cors::install(server& s) const {
    auto p = std::make_shared<cors_policy>();
    if (!origin_regex_.empty()) p->origin_regex = std::make_unique<std::regex>(origin_regex_, std::regex::ECMAScript);
    for (const auto& origin : origins_) {
        if (origin == "*") p->any_origin = true;
        else p->origins.insert(origin);
    }
    p->any_method = has(methods_, "*");
    p->any_header = has(headers_, "*");
    p->credentials = credentials_;
    for (const auto& m : methods_) p->methods.push_back(upper(m));
    p->allow_methods = p->any_method ? "DELETE, GET, HEAD, OPTIONS, PATCH, POST, PUT" : join(p->methods);
    // Always allowed by the CORS spec (the request-header safelist); listed first so a preflight sees them.
    std::vector<std::string> shown = {"Accept", "Accept-Language", "Content-Language", "Content-Type"};
    for (const auto& h : shown) p->header_set.insert(lower(h));
    for (const auto& h : headers_) {
        if (h == "*") continue;
        p->header_set.insert(lower(h));
        if (!has(shown, h)) shown.push_back(h);
    }
    p->allow_headers = join(shown);
    p->expose = join(expose_);
    p->max_age = std::to_string(max_age_);

    auto before = [p](const server_request& req, response& res) {
        if (!is_preflight(req)) return false;
        const std::string origin = req.header("Origin");
        std::string failures;
        auto fail = [&](const char* what) { failures += failures.empty() ? what : std::string(", ") + what; };
        if (!p->origin_ok(origin)) fail("origin");
        const std::string method = upper(trim(req.header("Access-Control-Request-Method")));
        if (!p->any_method && !has(p->methods, method)) fail("method");
        const std::string asked = req.header("Access-Control-Request-Headers");
        if (!p->any_header && !asked.empty()) {
            std::size_t pos = 0;
            bool bad = false;
            while (pos <= asked.size()) {
                std::size_t end = asked.find(',', pos);
                if (end == std::string::npos) end = asked.size();
                const std::string h = lower(trim(asked.substr(pos, end - pos)));
                if (!h.empty() && !p->header_set.count(h)) bad = true;
                pos = end + 1;
            }
            if (bad) fail("headers");
        }
        if (!failures.empty()) { bad_request(res, "Disallowed CORS " + failures); return true; }
        res.status = 200;
        res.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
        res.body = "OK";
        res.headers.emplace_back("Access-Control-Allow-Origin", p->origin_value(origin));
        if (p->credentials) res.headers.emplace_back("Access-Control-Allow-Credentials", "true");
        res.headers.emplace_back("Access-Control-Allow-Methods", p->allow_methods);
        res.headers.emplace_back("Access-Control-Allow-Headers", p->any_header && !asked.empty() ? asked : p->allow_headers);
        res.headers.emplace_back("Access-Control-Max-Age", p->max_age);
        if (p->vary_origin()) res.headers.emplace_back("Vary", "Origin");
        return true;
    };
    auto after = [p](const server_request& req, response& res) {
        if (is_preflight(req)) return; // answered (or refused) by the before hook
        const std::string origin = req.header("Origin");
        if (origin.empty()) return;
        if (p->vary_origin()) res.headers.emplace_back("Vary", "Origin");
        if (!p->origin_ok(origin)) return;
        res.headers.emplace_back("Access-Control-Allow-Origin", p->origin_value(origin));
        if (p->credentials) res.headers.emplace_back("Access-Control-Allow-Credentials", "true");
        if (!p->expose.empty()) res.headers.emplace_back("Access-Control-Expose-Headers", p->expose);
    };
    s.use(std::move(before), std::move(after));
}

trusted_host::trusted_host(std::vector<std::string> allowed_hosts) { set_allowed_hosts(std::move(allowed_hosts)); }

trusted_host& trusted_host::set_allowed_hosts(std::vector<std::string> hosts) {
    if (hosts.empty()) throw std::invalid_argument("trusted_host: allowed_hosts must not be empty");
    for (const auto& raw : hosts) {
        const std::string h = lower(raw);
        const bool wildcard = h == "*" || (h.compare(0, 2, "*.") == 0 && h.size() > 2 && h.find('*', 1) == std::string::npos);
        if (!wildcard && (h.empty() || h.find('*') != std::string::npos))
            throw std::invalid_argument("trusted_host: '" + raw + "' is not a host name, \"*\" or \"*.domain\"");
    }
    hosts_ = std::move(hosts);
    return *this;
}

trusted_host& trusted_host::set_www_redirect(bool redirect) { www_redirect_ = redirect; return *this; }

void trusted_host::install(server& s) const {
    auto p = std::make_shared<host_policy>();
    p->www_redirect = www_redirect_;
    for (const auto& raw : hosts_) {
        const std::string h = lower(raw);
        if (h == "*") p->any = true;
        else if (h.compare(0, 2, "*.") == 0) p->suffixes.push_back(h.substr(1));
        else p->exact.push_back(h);
    }
    s.use([p](const server_request& req, response& res) {
        const std::string header = req.header("Host");
        const std::string host = host_name(header);
        if (p->allowed(host)) return false;
        if (p->www_redirect && !host.empty()) {
            const std::string other = host.compare(0, 4, "www.") == 0 ? host.substr(4) : "www." + host;
            if (p->allowed(other)) {
                std::string port;
                const std::string h = lower(trim(header));
                if (const auto colon = h.rfind(':'); colon != std::string::npos && h.find(':') == colon) port = h.substr(colon);
                std::string target = req.scheme() + "://" + other + port + encode(req.path, true);
                char sep = '?';
                for (const auto& [k, v] : req.query) { target += sep; target += encode(k, false) + "=" + encode(v, false); sep = '&'; }
                res.redirect(target, 307);
                return true;
            }
        }
        bad_request(res, "Invalid host header");
        return true;
    });
}

https_redirect::https_redirect() = default;

https_redirect& https_redirect::set_trust_forwarded_proto(bool trust) { trust_ = trust; return *this; }

void https_redirect::install(server& s) const {
    const bool trust = trust_;
    s.use([trust](const server_request& req, response& res) {
        if (req.secure) return false;
        if (trust && lower(trim(req.header("X-Forwarded-Proto"))) == "https") return false;
        std::string host = trim(req.header("Host"));
        if (host.empty()) { bad_request(res, "Invalid host header"); return true; }
        // ponytail: the port of the Host header is kept, except the plain-HTTP default; a proxy that listens on 80 and 443 is the usual case.
        const auto colon = host.rfind(':');
        if (colon != std::string::npos && host.find(':') == colon && host.compare(colon, std::string::npos, ":80") == 0) host.resize(colon);
        std::string target = "https://" + host + encode(req.path, true);
        char sep = '?';
        for (const auto& [k, v] : req.query) { target += sep; target += encode(k, false) + "=" + encode(v, false); sep = '&'; }
        res.redirect(target, 307);
        return true;
    });
}

} // namespace httpp
