// request::form(): urlencoded and multipart/form-data bodies. Nothing here runs unless a handler asks for the form.

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

#include "httpp/server.hpp"
#include "internal/urlencoded.hpp"

namespace httpp {

namespace detail {

namespace {
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

std::string percent_decode(const std::string& in, bool plus) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '%' && i + 2 < in.size() && hex_value(in[i + 1]) >= 0 && hex_value(in[i + 2]) >= 0) {
            out += static_cast<char>(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2]));
            i += 2;
        } else if (c == '+' && plus) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

void parse_query(const std::string& q, std::vector<std::pair<std::string, std::string>>& out) {
    std::size_t pos = 0;
    while (pos <= q.size()) {
        std::size_t end = q.find('&', pos);
        if (end == std::string::npos) end = q.size();
        if (end > pos) {
            const std::string item = q.substr(pos, end - pos);
            const std::size_t eq = item.find('=');
            if (eq == std::string::npos) out.emplace_back(percent_decode(item, true), "");
            else out.emplace_back(percent_decode(item.substr(0, eq), true), percent_decode(item.substr(eq + 1), true));
        }
        pos = end + 1;
    }
}

}  // namespace detail

namespace {

constexpr std::size_t max_parts = 1024;          // fields and files together
constexpr std::size_t max_part_header = 8 * 1024; // the header block of one part
constexpr std::size_t max_boundary = 200;

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

// The value of `key` in "type; a=1; b=\"x y\"" (quotes and backslash escapes handled); found=false if absent.
std::string param(const std::string& header, const std::string& key, bool& found) {
    found = false;
    std::size_t i = header.find(';');
    while (i != std::string::npos && i < header.size()) {
        ++i;
        while (i < header.size() && (header[i] == ' ' || header[i] == '\t')) ++i;
        const std::size_t eq = header.find('=', i);
        const std::size_t semi_after_name = header.find(';', i);
        if (eq == std::string::npos || (semi_after_name != std::string::npos && semi_after_name < eq)) {
            i = semi_after_name;
            continue;
        }
        const std::string name = lower(trim(header.substr(i, eq - i)));
        std::string value;
        i = eq + 1;
        while (i < header.size() && (header[i] == ' ' || header[i] == '\t')) ++i;
        if (i < header.size() && header[i] == '"') {
            ++i;
            while (i < header.size() && header[i] != '"') {
                // Only \" is an escape: browsers send Windows paths with plain backslashes (see base_name).
                if (header[i] == '\\' && i + 1 < header.size() && header[i + 1] == '"') ++i;
                value += header[i++];
            }
            if (i < header.size()) ++i;  // closing quote
            i = header.find(';', i);
        } else {
            const std::size_t end = header.find(';', i);
            value = trim(header.substr(i, end == std::string::npos ? std::string::npos : end - i));
            i = end;
        }
        if (name == key) {
            found = true;
            return value;
        }
    }
    return "";
}

// Browsers may send a full path ("C:\\dir\\a.txt"): keep only the last component.
std::string base_name(const std::string& name) {
    const std::size_t cut = name.find_last_of("/\\");
    return cut == std::string::npos ? name : name.substr(cut + 1);
}

[[noreturn]] void bad(const char* why) { throw std::invalid_argument(std::string("malformed multipart/form-data: ") + why); }

form_data parse_multipart(const std::string& body, const std::string& boundary) {
    const std::string delim = "--" + boundary;
    const std::string sep = "\r\n" + delim;
    form_data out;

    std::size_t pos;
    if (body.compare(0, delim.size(), delim) == 0) pos = delim.size();
    else {
        const std::size_t first = body.find(sep);  // skip a preamble
        if (first == std::string::npos) bad("no boundary in the body");
        pos = first + sep.size();
    }
    for (;;) {
        if (body.compare(pos, 2, "--") == 0) return out;  // the closing delimiter
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;  // transport padding
        if (body.compare(pos, 2, "\r\n") != 0) bad("expected a line break after the boundary");
        pos += 2;

        // The header block ends at the first blank line. A part with no headers has it right at `pos - 2`.
        const std::size_t head_end = body.find("\r\n\r\n", pos - 2);
        if (head_end == std::string::npos) bad("a part has no end of headers");
        const std::string head = body.substr(pos, head_end + 2 - pos);
        if (head.size() > max_part_header) bad("a part header is too large");
        const std::size_t data_begin = head_end + 4;

        std::string disposition, content_type;
        std::size_t line = 0;
        while (line < head.size()) {
            std::size_t eol = head.find("\r\n", line);
            if (eol == std::string::npos) eol = head.size();
            const std::string text = head.substr(line, eol - line);
            line = eol + 2;
            const std::size_t colon = text.find(':');
            if (colon == std::string::npos) continue;
            const std::string name = lower(trim(text.substr(0, colon)));
            if (name == "content-disposition") disposition = trim(text.substr(colon + 1));
            else if (name == "content-type") content_type = trim(text.substr(colon + 1));
        }

        const std::size_t data_end = body.find(sep, data_begin);
        if (data_end == std::string::npos) bad("a part is not closed by a boundary");
        if (lower(disposition).compare(0, 9, "form-data") != 0) bad("a part has no form-data disposition");
        bool has_name = false, has_file = false;
        const std::string name = param(disposition, "name", has_name);
        const std::string filename = param(disposition, "filename", has_file);
        if (!has_name) bad("a part has no name");
        if (out.fields.size() + out.files.size() >= max_parts) bad("too many parts");

        std::string data = body.substr(data_begin, data_end - data_begin);
        if (has_file) {
            upload_file f;
            f.field = name;
            f.filename = base_name(filename);
            f.content_type = content_type.empty() ? "application/octet-stream" : content_type;
            f.data = std::move(data);
            out.files.push_back(std::move(f));
        } else {
            out.fields.emplace_back(name, std::move(data));
        }
        pos = data_end + sep.size();
    }
}

}  // namespace

form_data request::form() const {
    std::string type = header("Content-Type");
    const std::string media = lower(trim(type.substr(0, type.find(';'))));
    if (media == "application/x-www-form-urlencoded") {
        form_data out;
        detail::parse_query(body, out.fields);
        return out;
    }
    if (media == "multipart/form-data") {
        bool found = false;
        const std::string boundary = param(type, "boundary", found);
        if (!found || boundary.empty() || boundary.size() > max_boundary) bad("missing or invalid boundary");
        return parse_multipart(body, boundary);
    }
    return {};
}

}  // namespace httpp
