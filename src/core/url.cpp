#include "httpp/url.hpp"

// urlparser.h is included ONLY in this translation unit — never in a public
// httpp/*.hpp header — and stays hidden inside the compiled core.
#include <urlparser.h>

#include <stdexcept>

namespace httpp {

url url::parse(const std::string& raw) {
    url u;

    urlparser::url parsed;
    try {
        parsed = urlparser::url(raw);
    } catch (const std::exception&) {
        return u; // invalid: unparsable
    }

    const std::string scheme(parsed.protocol());
    int default_port = 0;
    if (scheme == "http") {
        default_port = 80;
    } else if (scheme == "https") {
        default_port = 443;
    } else {
        return u; // invalid: httpp only deals in http(s)
    }

    const std::string host(parsed.host_text());
    if (host.empty()) {
        return u; // invalid: no host
    }

    u.scheme_ = scheme;
    u.host_ = host;
    u.port_ = (parsed.port() != 0) ? parsed.port() : default_port;
    u.path_ = parsed.abspath().empty() ? "/" : parsed.abspath();
    u.query_ = std::string(parsed.query());
    u.valid_ = true;
    return u;
}

} // namespace httpp
