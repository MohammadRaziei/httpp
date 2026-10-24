#include "internal/response_compression.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

#include "internal/deflate.hpp"

namespace httpp::detail {
namespace {

bool ieq(const std::string& a, const char* b) {
    const std::size_t n = std::char_traits<char>::length(b);
    return a.size() == n && std::equal(a.begin(), a.end(), b, [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

// Accept-Encoding: "gzip, deflate;q=0.5, *;q=0". gzip is accepted when it (or *) is listed with q above 0;
// a direct "gzip" entry wins over "*".
bool accepts_gzip(const std::string& header) {
    int star = -1;
    std::size_t pos = 0;
    while (pos <= header.size()) {
        std::size_t end = header.find(',', pos);
        if (end == std::string::npos) end = header.size();
        const std::string item = header.substr(pos, end - pos);
        pos = end + 1;
        const std::size_t semi = item.find(';');
        const std::string name = trim(item.substr(0, semi));
        bool ok = true;
        if (semi != std::string::npos) {
            const std::string param = lower(trim(item.substr(semi + 1)));
            if (param.compare(0, 2, "q=") == 0) {
                const std::string q = trim(param.substr(2));
                ok = q.find_first_of("123456789") != std::string::npos;  // "0", "0.0", "0.000" are refusals
            }
        }
        if (ieq(name, "gzip") || ieq(name, "x-gzip")) return ok;
        if (name == "*") star = ok ? 1 : 0;
    }
    return star == 1;
}

bool compressible(const std::string& content_type) {
    const std::string t = trim(lower(content_type.substr(0, content_type.find(';'))));
    if (t.compare(0, 5, "text/") == 0) return true;
    static const char* const exact[] = {"application/json", "application/javascript", "application/xml",
                                        "application/xhtml+xml", "application/x-ndjson", "image/svg+xml"};
    for (const char* e : exact) if (t == e) return true;
    const auto ends = [&](const char* suffix) {
        const std::size_t n = std::char_traits<char>::length(suffix);
        return t.size() > n && t.compare(t.size() - n, n, suffix) == 0;
    };
    return ends("+json") || ends("+xml");
}

const std::string* find_header(const response& res, const char* name) {
    for (const auto& h : res.headers) if (ieq(h.first, name)) return &h.second;
    return nullptr;
}

}  // namespace

void response_compression::configure(std::size_t min_size, int level) {
    if (min_size != 0 && (level < 1 || level > 9)) throw std::invalid_argument("compression level must be in 1..9");
    min_size_ = min_size;
    level_ = level;
}

void response_compression::apply(const request& req, response& res) const {
    if (!enabled() || res.body.size() < min_size_ || !res.file.empty() || res.stream) return;
    if (res.status < 200 || res.status == 204 || res.status == 206 || res.status == 304) return;
    if (find_header(res, "Content-Encoding") || find_header(res, "Content-Range")) return;
    const std::string* type = find_header(res, "Content-Type");
    if (!type || !compressible(*type)) return;

    // The answer depends on Accept-Encoding from here on, so caches must keep the versions apart.
    if (const std::string* vary = find_header(res, "Vary")) {
        if (lower(*vary).find("accept-encoding") == std::string::npos && *vary != "*")
            for (auto& h : res.headers) if (ieq(h.first, "Vary")) h.second += ", Accept-Encoding";
    } else {
        res.headers.emplace_back("Vary", "Accept-Encoding");
    }
    if (!accepts_gzip(req.header("Accept-Encoding"))) return;

    std::string z = deflate(res.body.data(), res.body.size(), inflate_format::gzip, level_);
    if (z.size() >= res.body.size()) return;  // incompressible data: keep the original
    res.body = std::move(z);
    res.headers.emplace_back("Content-Encoding", "gzip");
}

}  // namespace httpp::detail
