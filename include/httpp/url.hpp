#pragma once

#include <string>

namespace httpp {

// Minimal HTTP(S) URL parser. Header-only, no system/third-party deps.
class url {
public:
    static url parse(const std::string& raw) {
        url u;

        const auto scheme_end = raw.find("://");
        if (scheme_end == std::string::npos) {
            return u; // invalid: no scheme
        }

        const std::string scheme = raw.substr(0, scheme_end);
        int default_port = 0;
        if (scheme == "http") {
            default_port = 80;
        } else if (scheme == "https") {
            default_port = 443;
        } else {
            return u; // invalid: unsupported scheme
        }

        const std::size_t authority_start = scheme_end + 3;
        std::size_t path_start = raw.find('/', authority_start);
        const std::string authority = raw.substr(
            authority_start,
            (path_start == std::string::npos ? raw.size() : path_start) - authority_start);

        if (authority.empty()) {
            return u; // invalid: no host
        }

        std::string host = authority;
        int port = default_port;
        const auto colon = authority.find(':');
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            port = std::stoi(authority.substr(colon + 1));
        }
        if (host.empty()) {
            return u; // invalid: no host
        }

        std::string rest = (path_start == std::string::npos) ? "" : raw.substr(path_start);
        std::string path = "/";
        std::string query;
        if (!rest.empty()) {
            const auto q = rest.find('?');
            path = (q == std::string::npos) ? rest : rest.substr(0, q);
            if (q != std::string::npos) {
                query = rest.substr(q + 1);
            }
        }

        u.scheme_ = scheme;
        u.host_ = host;
        u.port_ = port;
        u.path_ = path;
        u.query_ = query;
        u.valid_ = true;
        return u;
    }

    bool valid() const { return valid_; }
    const std::string& scheme() const { return scheme_; }
    const std::string& host() const { return host_; }
    int port() const { return port_; }
    const std::string& path() const { return path_; }
    const std::string& query() const { return query_; }

private:
    bool valid_ = false;
    std::string scheme_;
    std::string host_;
    int port_ = 0;
    std::string path_;
    std::string query_;
};

} // namespace httpp
