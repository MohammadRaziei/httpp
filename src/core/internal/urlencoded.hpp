#pragma once

// Internal. Percent decoding and "a=1&b=2" parsing, shared by the router's query handling and by request::form().

#include <string>
#include <utility>
#include <vector>

namespace httpp::detail {

// %XX decoding; a malformed escape is kept as is. `plus` turns '+' into a space (query strings, form bodies).
std::string percent_decode(const std::string& in, bool plus);

// "a=1&b=%20&c" -> [(a,1),(b," "),(c,"")], appended to `out`. Empty items are skipped.
void parse_query(const std::string& q, std::vector<std::pair<std::string, std::string>>& out);

}  // namespace httpp::detail
