#pragma once

// Internal. Percent decoding and "a=1&b=2" parsing, shared by the router's query handling and by request::form().

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace httpp::detail {

// %XX decoding; a malformed escape is kept as is. `plus` turns '+' into a space (query strings, form bodies).
std::string percent_decode(const std::string& in, bool plus);

// Most pairs one query string or one urlencoded body may hold (a pair costs about 56 bytes of memory per body byte of "x=1&", so the count is
// the limit that matters). The same number as the parts of a multipart body; Django and Tomcat use 1000 and 10000.
constexpr std::size_t max_query_items = 1024;

// "a=1&b=%20&c" -> [(a,1),(b," "),(c,"")], appended to `out`. Empty items are skipped. Returns false, with `out` holding the pairs
// read so far, as soon as there are more than `max_items` of them (0 = no limit).
bool parse_query(const std::string& q, std::vector<std::pair<std::string, std::string>>& out, std::size_t max_items = max_query_items);

}  // namespace httpp::detail
