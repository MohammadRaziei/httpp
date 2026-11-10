#include "httpp/file_browser.hpp"

#include "httpp_file_browser_data.hpp" // generated at configure time, see CMakeLists.txt
#include "internal/fs.hpp"
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

// ---- the file system (httpp::fs, ADR-0054) ----
// `root` is always a real() path: links resolved, no trailing '/' (except the file system root itself).

bool resolve_dir(const std::string& root, const std::string& rel, std::string& full) { // full: the folder, links resolved; false when it is missing, not a folder or outside
    const std::string real = fs::real(rel == "/" ? root : root + rel);
    if (real.empty() || !fs::is_inside(real, root)) return false;
    if (fs::stat(real).type != fs::kind::directory) return false;
    full = real;
    return true;
}

std::string canonical_root(const std::string& root, bool& is_fs_root) {
    const std::string real = fs::real(root);
    if (real.empty() || fs::stat(real).type != fs::kind::directory) return "";
    is_fs_root = fs::is_root(real);
    return real;
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
    std::vector<std::string> names;
    if (!fs::list(dir, names)) return false;
    for (const std::string& name : names) {
        if (!show_hidden && name[0] == '.') continue;
        if (!valid_utf8(name)) continue;
        const std::string path = fs::join(dir, name);
        fs::info st = fs::lstat(path);
        if (st.type == fs::kind::missing && !st.link) continue;
        if (st.link) { // a link counts only when it stays inside the root
            const std::string real = fs::real(path);
            if (real.empty() || !fs::is_inside(real, root)) continue;
            st = fs::stat(path);
        }
        item i;
        i.name = name;
        if (st.type == fs::kind::directory) i.dir = true;
        else if (st.type == fs::kind::file) i.size = st.size;
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

void mount_file_browser(server& s, const std::string& base, const path& root_dir, const file_browser_options& o) {
    const std::string& root = root_dir.str();
    std::string prefix = base;
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back(); // "/" -> "" (the root of the server)
    if (base.empty() || base[0] != '/') throw std::invalid_argument("mount_file_browser: base must start with '/'");
    if (o.theme != "auto" && o.theme != "light" && o.theme != "dark") throw std::invalid_argument("mount_file_browser: theme must be auto, light or dark");
    if (o.view != "list" && o.view != "grid") throw std::invalid_argument("mount_file_browser: view must be list or grid");
    if (o.sort != "name" && o.sort != "size" && o.sort != "date") throw std::invalid_argument("mount_file_browser: sort must be name, size or date");
    if (!o.accent.empty() && !is_hex_colour(o.accent)) throw std::invalid_argument("mount_file_browser: accent must be #rrggbb");
    if (o.max_entries == 0) throw std::invalid_argument("mount_file_browser: max_entries must be above 0");
    bool is_fs_root = false;
    const std::string real_root = root.empty() ? std::string() : canonical_root(root, is_fs_root);
    if (real_root.empty()) throw std::invalid_argument("mount_file_browser: root is not a folder");
    if (is_fs_root) throw std::invalid_argument("mount_file_browser: the root of the file system is not served");

    // The page: the embedded HTML with the title and the options filled in (the root path on disk is never in it).
    std::string html = embedded::file_browser_html_is_gzip
        ? detail::inflate(embedded::file_browser_html, embedded::file_browser_html_size, detail::inflate_format::gzip, embedded::file_browser_html_raw_size + 1)
        : std::string(reinterpret_cast<const char*>(embedded::file_browser_html), embedded::file_browser_html_size);
    const std::string config = "{\"base\":" + json_string(prefix) + ",\"theme\":" + json_string(o.theme) + ",\"accent\":" + json_string(o.accent) +
                               ",\"search\":" + (o.search ? "true" : "false") + ",\"view\":" + json_string(o.view) + ",\"sort\":" + json_string(o.sort) +
                               ",\"limit\":" + std::to_string(o.max_entries) + "}";
    replace_all(html, "__CONFIG__", config);
    replace_all(html, "__TITLE__", html_escape(o.title));

    auto page = [html](const request&, response& res) {
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

    const bool show_hidden = o.show_hidden;
    const std::size_t limit = o.max_entries;
    s.get(prefix + "/api/list", [real_root, show_hidden, limit](const request& req, response& res) {
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

    server::directory_options dopt;
    dopt.index = false;
    dopt.hidden = show_hidden;
    dopt.download_active = true;
    s.serve_directory(prefix + "/raw", real_root, dopt);
}

} // namespace httpp
