#include "httpp/url.hpp"

#include "httpp/client.hpp"

#include <stdexcept>

namespace httpp {

std::string encode_component(const std::string& text) {
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

std::string build_query(const std::vector<std::pair<std::string, std::string>>& params) {
    std::string out;
    for (const auto& [name, value] : params) {
        if (!out.empty()) out.push_back('&');
        out += encode_component(name);
        out.push_back('=');
        out += encode_component(value);
    }
    return out;
}

std::string decode_component(const std::string& text) {
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

void fetch_public_suffix_list(const std::string& source, long timeout_seconds) {
    const std::string from = source.empty() ? std::string(urlparser::psl::instance().source_url()) : source;
    const response res = request(from).timeout(timeout_seconds).max_redirects(20).run();
    if (res.failed()) {
        throw std::runtime_error("fetch_public_suffix_list: " + from + ": " + res.error_name() + ": " + res.error_message);
    }
    if (res.status != 200) {
        throw std::runtime_error("fetch_public_suffix_list: " + from + ": HTTP status " + std::to_string(res.status));
    }
    const auto first = res.body.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        throw std::runtime_error("fetch_public_suffix_list: " + from + ": empty answer");
    }
    if (res.body[first] == '<') { // a 200 with a web page (a captive portal, an error page) would otherwise parse as rules
        throw std::runtime_error("fetch_public_suffix_list: " + from + ": not a Public Suffix List (got a web page)");
    }
    try {
        urlparser::psl::instance().load_from_string(res.body); // replaces the list only when it parsed
    } catch (const std::exception& e) {
        throw std::runtime_error("fetch_public_suffix_list: " + from + ": not a Public Suffix List: " + e.what());
    }
}

} // namespace httpp
