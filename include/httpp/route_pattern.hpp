// The one grammar for route paths, used by the C++ router and (through the Cython binding) by Python.
//
//   /users/{id}          a path parameter: any non-empty segment, captured as "id"
//   /users/{id:int}      only a whole number fits ("-5", "42"); "abc" does not match this route
//   /prices/{p:float}    a number with an optional fraction ("3", "-3.5")
//
// A parameter is always a whole segment. Anything that looks like a parameter but is not one of the forms
// above (":id", "<id>", "{id", "{}", "a{id}", an unknown type) is an error at registration, never a literal
// that silently never matches.
#pragma once

#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace httpp {

enum class param_type { string, integer, number };

struct route_segment {
    bool is_param = false;
    std::string text;                      // the literal text, or the parameter's name
    param_type type = param_type::string;  // only for a parameter
};

namespace detail {

inline bool valid_param_name(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (const unsigned char c : s)
        if (!(std::isalnum(c) || c == '_')) return false;
    return true;
}

}  // namespace detail

// Does `text` (an already percent-decoded segment) fit a parameter of this type?
inline bool param_fits(const std::string& text, param_type t) {
    if (t == param_type::string) return !text.empty();
    std::size_t i = (!text.empty() && text[0] == '-') ? 1 : 0;
    const std::size_t digits_from = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    if (i == digits_from) return false;                  // no digits at all
    if (t == param_type::integer) return i == text.size() && i - digits_from <= 18;  // fits a 64-bit integer
    if (i == text.size()) return true;
    if (text[i] != '.') return false;
    const std::size_t frac_from = ++i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
    return i == text.size() && i > frac_from;
}

// "/a/{id:int}/b" -> its segments. `path` starts with '/'. Throws std::invalid_argument on a malformed pattern.
inline std::vector<route_segment> parse_route_pattern(const std::string& path) {
    auto bad = [&](const std::string& seg, const std::string& why) {
        return std::invalid_argument("route '" + path + "': segment '" + seg + "' " + why);
    };
    std::vector<route_segment> out;
    std::size_t pos = path.empty() || path[0] != '/' ? 0 : 1;
    while (pos <= path.size()) {
        std::size_t end = path.find('/', pos);
        if (end == std::string::npos) end = path.size();
        const std::string seg = path.substr(pos, end - pos);
        pos = end + 1;

        route_segment s;
        const bool braces = seg.find_first_of("{}") != std::string::npos;
        if (seg.size() > 1 && seg[0] == ':') throw bad(seg, "uses the old \":name\" form; write {name}");
        if (!seg.empty() && (seg.front() == '<' || seg.back() == '>')) throw bad(seg, "uses the \"<name>\" form; write {name}");
        if (!braces) {
            s.text = seg;
            out.push_back(std::move(s));
            continue;
        }
        if (seg.size() < 3 || seg.front() != '{' || seg.back() != '}' || seg.find_first_of("{}", 1) != seg.size() - 1)
            throw bad(seg, "is not a whole-segment parameter like {name} or {name:int}");
        std::string name = seg.substr(1, seg.size() - 2);
        const std::size_t colon = name.find(':');
        if (colon != std::string::npos) {
            const std::string type = name.substr(colon + 1);
            name.resize(colon);
            if (type == "int") s.type = param_type::integer;
            else if (type == "float") s.type = param_type::number;
            else throw bad(seg, "has the unknown type '" + type + "' (use int or float)");
        }
        if (!detail::valid_param_name(name)) throw bad(seg, "has an invalid parameter name (letters, digits and '_', not starting with a digit)");
        s.is_param = true;
        s.text = std::move(name);
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace httpp
