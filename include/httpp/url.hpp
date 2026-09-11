#pragma once

#include "httpp/export.hpp"

#include <string>

namespace httpp {

// httpp's own thin, http(s)-specific view of a URL: valid()/scheme()/host()/
// port()/path()/query(). The actual parsing (IPv4/IPv6/PSL-aware, RFC-ish)
// is done by the vendored liburlparser (src/third_party/liburlparser, a git
// submodule built from source — no system package). That dependency is
// used only in src/core/url.cpp and never appears in this public header.
class HTTPP_API url {
public:
    static url parse(const std::string& raw);

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
