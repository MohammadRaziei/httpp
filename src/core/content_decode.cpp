#include "internal/content_decode.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "internal/inflate.hpp"

namespace httpp::detail {
namespace {

std::string lower_trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    std::string out = s.substr(a, b - a);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

bool name_is(const std::string& header, const char* wanted) { return lower_trim(header) == wanted; }

// The codings in the order they were applied. Several headers and comma lists both count.
std::vector<std::string> codings(const response& res) {
    std::vector<std::string> out;
    for (const auto& [name, value] : res.headers) {
        if (!name_is(name, "content-encoding")) continue;
        std::size_t pos = 0;
        while (pos <= value.size()) {
            std::size_t comma = value.find(',', pos);
            if (comma == std::string::npos) comma = value.size();
            std::string token = lower_trim(value.substr(pos, comma - pos));
            if (!token.empty()) out.push_back(std::move(token));
            pos = comma + 1;
        }
    }
    return out;
}

} // namespace

void decode_content(response& res, std::size_t max_bytes) {
    if (res.error != error_kind::none || res.body.empty()) return;
    std::vector<std::string> list = codings(res);
    list.erase(std::remove(list.begin(), list.end(), "identity"), list.end());
    if (list.empty()) return;
    for (const auto& c : list)
        if (c != "gzip" && c != "x-gzip" && c != "deflate") return; // not ours to decode: leave the body as it is

    const std::size_t limit = max_bytes > 0 ? max_bytes : static_cast<std::size_t>(-1); // inflate clamps to 2 GiB - 1
    std::string body = std::move(res.body);
    res.body.clear();
    try {
        for (auto it = list.rbegin(); it != list.rend(); ++it) { // the last coding applied is the first to undo
            const inflate_format f = (*it == "deflate") ? inflate_format::deflate : inflate_format::gzip;
            body = inflate(body.data(), body.size(), f, limit);
        }
    } catch (const inflate_limit_error&) {
        res.status = 413;
        res.error = error_kind::too_large;
        res.error_message = "decoded response body exceeded max_response_size";
        return;
    } catch (const inflate_error& e) {
        res.status = 0;
        res.error = error_kind::other;
        res.error_message = std::string("could not decode the ") + list.front() + " response body: " + e.what();
        return;
    } catch (const std::bad_alloc&) {
        res.status = 0;
        res.error = error_kind::other;
        res.error_message = "not enough memory to decode the response body";
        return;
    }
    res.body = std::move(body);
}

} // namespace httpp::detail
