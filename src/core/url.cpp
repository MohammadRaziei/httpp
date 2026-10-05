#include "httpp/url.hpp"

// urlparser.h is included ONLY in this translation unit — never in a public
// httpp/*.hpp header — and stays hidden inside the compiled core.
#include <urlparser.h>

#include <stdexcept>

namespace httpp {

std::string url::encode_component(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

std::string url::build_query(const std::vector<std::pair<std::string, std::string>>& params) {
    std::string out;
    for (const auto& [name, value] : params) {
        if (!out.empty()) out.push_back('&');
        out += encode_component(name);
        out.push_back('=');
        out += encode_component(value);
    }
    return out;
}

std::string url::decode_component(const std::string& text) {
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '%' && i + 2 < text.size() + 0 && hexval(text[i + 1]) >= 0 && hexval(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hexval(text[i + 1]) * 16 + hexval(text[i + 2])));
            i += 2;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

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
