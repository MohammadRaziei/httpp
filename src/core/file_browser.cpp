#include "httpp/file_browser.hpp"

#include "httpp_file_browser_data.hpp" // generated at configure time, see CMakeLists.txt
#include "internal/inflate.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace httpp {

namespace {

struct item {
    std::string name;
    bool dir = false;
    long long size = 0;
    long long mtime = 0;
};

// ---- text helpers ----

bool valid_utf8(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) { i++; continue; }
        std::size_t len;
        unsigned cp;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else return false;
        if (i + len > n) return false;
        for (std::size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

// A JSON string that is also safe inside an inline <script>: '<' is escaped, so "</script>" cannot appear.
std::string json_string(const std::string& in) {
    static const char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == '<' || c == '>' || c == '&' || c == 0x7f) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
                else out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

std::string html_escape(const std::string& in) {
    std::string out;
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
}

bool is_hex_colour(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (std::size_t i = 1; i < 7; i++) if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

// ---- the file system (httpp::path, ADR-0054, 0058) ----
// `root` is always a resolve() result: links resolved, no trailing '/' (except the file system root itself).

bool resolve_dir(const std::string& root, const std::string& rel, std::string& full) { // full: the folder, links resolved; false when it is missing, not a folder or outside
    const httpp::path real = httpp::path(rel == "/" ? root : root + rel).resolve();
    if (real.empty() || !real.is_relative_to(root)) return false;
    if (real.stat().type != httpp::path::kind::directory) return false;
    full = real.str();
    return true;
}

std::string canonical_root(const std::string& root, bool& is_fs_root) {
    const httpp::path real = httpp::path(root).resolve();
    if (real.empty() || real.stat().type != httpp::path::kind::directory) return "";
    is_fs_root = real.is_root();
    return real.str();
}

#ifdef _WIN32
bool segment_ok(const std::string& seg) { // ponytail: ':' (drives, streams) and the reserved device names are refused outright
    if (seg.find_first_of(":*?\"<>|") != std::string::npos) return false;
    std::string up;
    for (char c : seg.substr(0, seg.find('.'))) up += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const char* reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (const char* r : reserved) if (up == r) return false;
    return !seg.empty() && seg.back() != ' ' && seg.back() != '.';
}
#else
bool segment_ok(const std::string&) { return true; }
#endif

// Reads `dir`, keeps what is visible and inside `root`, stops after `limit` + 1 kept items.
bool read_dir(const std::string& root, const std::string& dir, bool show_hidden, std::size_t limit, std::vector<item>& out) {
    std::vector<httpp::path> entries;
    if (!httpp::path(dir).iterdir(entries)) return false;
    for (const httpp::path& entry : entries) {
        const std::string name = entry.name().str();
        if (!show_hidden && name[0] == '.') continue;
        if (!valid_utf8(name)) continue;
        httpp::path::status st = entry.lstat();
        if (st.type == httpp::path::kind::missing && !st.link) continue;
        if (st.link) { // a link counts only when it stays inside the root
            const httpp::path real = entry.resolve();
            if (real.empty() || !real.is_relative_to(root)) continue;
            st = entry.stat();
        }
        item i;
        i.name = name;
        if (st.type == httpp::path::kind::directory) i.dir = true;
        else if (st.type == httpp::path::kind::file) i.size = st.size;
        else continue; // sockets, pipes and devices are not files to browse
        i.mtime = st.mtime;
        out.push_back(std::move(i));
        if (out.size() > limit) break;
    }
    return true;
}

// ---- the request path ----

enum class path_result { ok, bad, not_found };

// "/a//b/" -> "/a/b". 400 for what is plainly an attack (".." and ".", backslash, NUL, bad Windows names), 404 for hidden names.
path_result clean_path(const std::string& in, bool show_hidden, std::string& rel) {
    if (in.find('\0') != std::string::npos || in.find('\\') != std::string::npos) return path_result::bad;
    std::string out;
    std::size_t pos = 0;
    while (pos <= in.size()) {
        std::size_t end = in.find('/', pos);
        if (end == std::string::npos) end = in.size();
        const std::string seg = in.substr(pos, end - pos);
        pos = end + 1;
        if (seg.empty()) continue;
        if (seg == "." || seg == "..") return path_result::bad;
        if (!segment_ok(seg)) return path_result::bad;
        if (!show_hidden && seg[0] == '.') return path_result::not_found;
        out += "/" + seg;
    }
    rel = out.empty() ? "/" : out;
    return path_result::ok;
}

response json_error(int status, const char* text) {
    response r;
    r.status = status;
    r.headers.emplace_back("Content-Type", "application/json");
    r.headers.emplace_back("Cache-Control", "no-store");
    r.headers.emplace_back("X-Content-Type-Options", "nosniff");
    r.body = std::string("{\"error\":\"") + text + "\"}"; // the same text for missing, hidden and outside
    return r;
}

} // namespace

static const char* theme_name(file_browser::theme_mode m) {
    switch (m) {
        case file_browser::theme_mode::light: return "light";
        case file_browser::theme_mode::dark: return "dark";
        default: return "auto";
    }
}
static const char* view_name(file_browser::view_mode v) { return v == file_browser::view_mode::grid ? "grid" : "list"; }
static const char* sort_name(file_browser::sort_key k) {
    switch (k) {
        case file_browser::sort_key::size: return "size";
        case file_browser::sort_key::date: return "date";
        default: return "name";
    }
}

file_browser::file_browser(path root) : root_(std::move(root)) {}

file_browser& file_browser::set_title(std::string title) { title_ = std::move(title); return *this; }
file_browser& file_browser::set_theme(theme_mode theme) { theme_ = theme; return *this; }
file_browser& file_browser::set_accent(std::string colour) {
    if (!colour.empty() && !is_hex_colour(colour)) throw std::invalid_argument("file_browser: accent must be #rrggbb");
    accent_ = std::move(colour);
    return *this;
}
file_browser& file_browser::set_search(bool on) { search_ = on; return *this; }
file_browser& file_browser::set_show_hidden(bool on) { show_hidden_ = on; return *this; }
file_browser& file_browser::set_safe_mode(bool on) { safe_mode_ = on; return *this; }
file_browser& file_browser::set_max_entries(std::size_t count) {
    if (count == 0) throw std::invalid_argument("file_browser: max_entries must be above 0");
    max_entries_ = count;
    return *this;
}
file_browser& file_browser::set_view(view_mode view) { view_ = view; return *this; }
file_browser& file_browser::set_sort(sort_key sort) { sort_ = sort; return *this; }

void file_browser::mount(server& s, const std::string& base) const {
    const std::string& root = root_.str();
    std::string prefix = base;
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back(); // "/" -> "" (the root of the server)
    if (base.empty() || base[0] != '/') throw std::invalid_argument("file_browser: base must start with '/'");
    bool is_fs_root = false;
    const std::string real_root = root.empty() ? std::string() : canonical_root(root, is_fs_root);
    if (real_root.empty()) throw std::invalid_argument("file_browser: root is not a folder");
    if (is_fs_root) throw std::invalid_argument("file_browser: the root of the file system is not served");

    // The page: the embedded HTML with the title and the options filled in (the root path on disk is never in it).
    std::string html = embedded::file_browser_html_is_gzip
        ? detail::inflate(embedded::file_browser_html, embedded::file_browser_html_size, detail::inflate_format::gzip, embedded::file_browser_html_raw_size + 1)
        : std::string(reinterpret_cast<const char*>(embedded::file_browser_html), embedded::file_browser_html_size);
    const std::string config = "{\"base\":" + json_string(prefix) + ",\"theme\":" + json_string(theme_name(theme_)) + ",\"accent\":" + json_string(accent_) +
                               ",\"search\":" + (search_ ? "true" : "false") + ",\"view\":" + json_string(view_name(view_)) + ",\"sort\":" + json_string(sort_name(sort_)) +
                               ",\"limit\":" + std::to_string(max_entries_) +
                               ",\"safe\":" + (safe_mode_ ? "true" : "false") + "}";
    replace_all(html, "__CONFIG__", config);
    replace_all(html, "__TITLE__", html_escape(title_));

    auto page = [html](const server_request&, response& res) {
        res.status = 200;
        res.headers.emplace_back("Content-Type", "text/html; charset=utf-8");
        res.headers.emplace_back("Content-Security-Policy",
            "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; img-src 'self' data:; media-src 'self'; connect-src 'self'; "
            "base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
        res.headers.emplace_back("X-Content-Type-Options", "nosniff");
        res.headers.emplace_back("Referrer-Policy", "no-referrer");
        res.body = html;
    };
    if (prefix.empty()) s.get("/", page);
    else { s.get(prefix, page); s.get(prefix + "/", page); }

    const bool show_hidden = show_hidden_;
    const std::size_t limit = max_entries_;
    s.get(prefix + "/api/list", [real_root, show_hidden, limit](const server_request& req, response& res) {
        std::string rel, dir;
        const path_result pr = clean_path(req.query_param("path"), show_hidden, rel);
        if (pr == path_result::bad) { res = json_error(400, "bad path"); return; }
        std::vector<item> items;
        if (pr != path_result::ok || !resolve_dir(real_root, rel, dir) || !read_dir(real_root, dir, show_hidden, limit, items)) {
            res = json_error(404, "not found");
            return;
        }
        const bool truncated = items.size() > limit;
        if (truncated) items.resize(limit);
        auto lower = [](const std::string& n) { std::string l = n; for (char& c : l) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); return l; };
        std::sort(items.begin(), items.end(), [&](const item& a, const item& b) {
            if (a.dir != b.dir) return a.dir;
            const std::string la = lower(a.name), lb = lower(b.name);
            return la != lb ? la < lb : a.name < b.name;
        });
        std::string body = "{\"path\":" + json_string(rel) + ",\"entries\":[";
        for (std::size_t i = 0; i < items.size(); i++) {
            if (i) body += ',';
            body += "{\"name\":" + json_string(items[i].name) + ",\"type\":\"" + (items[i].dir ? "dir" : "file") + "\",\"size\":" + std::to_string(items[i].size) +
                    ",\"mtime\":" + std::to_string(items[i].mtime) + "}";
        }
        body += std::string("],\"truncated\":") + (truncated ? "true" : "false") + ",\"limit\":" + std::to_string(limit) + "}";
        res.status = 200;
        res.headers.emplace_back("Content-Type", "application/json");
        res.headers.emplace_back("Cache-Control", "no-store");
        res.headers.emplace_back("X-Content-Type-Options", "nosniff");
        res.body = std::move(body);
    });

    directory(path(real_root)).set_index(false).set_show_hidden(show_hidden_).set_download_active(true).set_inline_active(!safe_mode_).mount(s, prefix + "/raw");
}

} // namespace httpp
