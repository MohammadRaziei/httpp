// The Authorization: Basic header (RFC 7617), shared by server_request::basic_auth and the basic_auth middleware.
#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace httpp::detail {

// Strict base64 (RFC 4648, padding required, no whitespace); false for anything else.
inline bool base64_decode(const std::string& in, std::string& out) {
    out.clear();
    if (in.empty() || in.size() % 4 != 0) return false;
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    for (std::size_t i = 0; i < in.size(); i += 4) {
        const bool last = i + 4 == in.size();
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            const char c = in[i + k];
            if (c == '=') {
                if (!last || k < 2) return false;
                v[k] = 0;
                pad++;
            } else {
                if (pad) return false; // data after padding
                v[k] = value(c);
                if (v[k] < 0) return false;
            }
        }
        const unsigned triple = (static_cast<unsigned>(v[0]) << 18) | (static_cast<unsigned>(v[1]) << 12) | (static_cast<unsigned>(v[2]) << 6) | static_cast<unsigned>(v[3]);
        out += static_cast<char>((triple >> 16) & 0xFF);
        if (pad < 2) out += static_cast<char>((triple >> 8) & 0xFF);
        if (pad < 1) out += static_cast<char>(triple & 0xFF);
    }
    return true;
}

// "Basic <base64 of user:password>" -> user, password. False for a missing header, another scheme, bad base64 or no ':'.
inline bool parse_basic_authorization(const std::string& header, std::string& user, std::string& password) {
    std::size_t a = 0, b = header.size();
    while (a < b && (header[a] == ' ' || header[a] == '\t')) a++;
    while (b > a && (header[b - 1] == ' ' || header[b - 1] == '\t')) b--;
    if (b - a < 7) return false;
    const char* scheme = "basic";
    for (std::size_t i = 0; i < 5; i++)
        if (std::tolower(static_cast<unsigned char>(header[a + i])) != scheme[i]) return false;
    if (header[a + 5] != ' ' && header[a + 5] != '\t') return false;
    std::size_t t = a + 6;
    while (t < b && (header[t] == ' ' || header[t] == '\t')) t++;
    std::string decoded;
    if (!base64_decode(header.substr(t, b - t), decoded)) return false;
    const auto colon = decoded.find(':');
    if (colon == std::string::npos) return false;
    user = decoded.substr(0, colon);
    password = decoded.substr(colon + 1);
    return true;
}

} // namespace httpp::detail
