#include "httpp/middleware.hpp"

#include <algorithm>
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

bool is_preflight(const request& r) {
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

} // namespace

void add_cors(server& s, const cors_options& o) {
    if (o.max_age < 0) throw std::invalid_argument("add_cors: max_age must not be negative");
    auto p = std::make_shared<cors_policy>();
    if (!o.allow_origin_regex.empty()) {
        try {
            p->origin_regex = std::make_unique<std::regex>(o.allow_origin_regex, std::regex::ECMAScript);
        } catch (const std::regex_error& e) {
            throw std::invalid_argument(std::string("add_cors: bad allow_origin_regex: ") + e.what());
        }
    }
    for (const auto& origin : o.allow_origins) {
        if (origin == "*") p->any_origin = true;
        else p->origins.insert(origin);
    }
    p->any_method = has(o.allow_methods, "*");
    p->any_header = has(o.allow_headers, "*");
    p->credentials = o.allow_credentials;
    for (const auto& m : o.allow_methods) p->methods.push_back(upper(m));
    p->allow_methods = p->any_method ? "DELETE, GET, HEAD, OPTIONS, PATCH, POST, PUT" : join(p->methods);
    // Always allowed by the CORS spec (the request-header safelist); listed first so a preflight sees them.
    std::vector<std::string> shown = {"Accept", "Accept-Language", "Content-Language", "Content-Type"};
    for (const auto& h : shown) p->header_set.insert(lower(h));
    for (const auto& h : o.allow_headers) {
        if (h == "*") continue;
        p->header_set.insert(lower(h));
        if (!has(shown, h)) shown.push_back(h);
    }
    p->allow_headers = join(shown);
    p->expose = join(o.expose_headers);
    p->max_age = std::to_string(o.max_age);

    auto before = [p](const request& req, response& res) {
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
    auto after = [p](const request& req, response& res) {
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

void add_trusted_host(server& s, const trusted_host_options& o) {
    if (o.allowed_hosts.empty()) throw std::invalid_argument("add_trusted_host: allowed_hosts must not be empty");
    auto p = std::make_shared<host_policy>();
    p->www_redirect = o.www_redirect;
    for (const auto& raw : o.allowed_hosts) {
        const std::string h = lower(raw);
        if (h == "*") p->any = true;
        else if (h.compare(0, 2, "*.") == 0 && h.size() > 2 && h.find('*', 1) == std::string::npos) p->suffixes.push_back(h.substr(1));
        else if (h.empty() || h.find('*') != std::string::npos)
            throw std::invalid_argument("add_trusted_host: '" + raw + "' is not a host name, \"*\" or \"*.domain\"");
        else p->exact.push_back(h);
    }
    s.use([p](const request& req, response& res) {
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

void add_https_redirect(server& s, const https_redirect_options& o) {
    const bool trust = o.trust_forwarded_proto;
    s.use([trust](const request& req, response& res) {
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
