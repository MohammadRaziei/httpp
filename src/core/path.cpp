#include "httpp/path.hpp"

#include "internal/fs.hpp"

#include <cctype>
#include <iterator>
#include <stdexcept>

namespace httpp {

std::string path::clean(std::string text) {
#ifdef _WIN32
    for (auto& c : text) if (c == '\\') c = '/';
#endif
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '/' && !out.empty() && out.back() == '/') continue; // repeated separators
        out += c;
    }
    while (out.size() > 1 && out.back() == '/' && !(out.size() == 3 && out[1] == ':')) out.pop_back();
    return out;
}

#ifdef _WIN32
std::wstring path::native() const { return fs::native(text_); }
#endif

path path::cwd() { return path(fs::cwd()); }

path path::absolute() const { return is_absolute() ? *this : cwd() / *this; }

std::string path::as_uri() const {
    static const char* hex = "0123456789ABCDEF";
    const std::string abs = absolute().text_;
    std::string out = "file://";
    if (abs.empty() || abs[0] != '/') out += '/'; // "C:/a" -> "file:///C:/a"
    for (unsigned char c : abs) {
        if (std::isalnum(c) || c == '/' || c == ':' || c == '-' || c == '.' || c == '_' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

bool path::exists() const { return fs::stat(text_).type != fs::kind::missing; }
bool path::is_file() const { return fs::stat(text_).type == fs::kind::file; }
bool path::is_dir() const { return fs::stat(text_).type == fs::kind::directory; }

long long path::size() const {
    const auto i = fs::stat(text_);
    return i.type == fs::kind::file ? i.size : -1;
}

bool path::mkdir() const { return fs::make_dir(text_) != fs::made::failed; }
bool path::unlink() const { return fs::remove_file(text_); }
bool path::rename(const path& to) const { return fs::replace(text_, to.text_); }

std::string path::read_text() const {
    std::ifstream in(native(), std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + text_);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void path::write_text(const std::string& data) const {
    std::ofstream out(native(), std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + text_);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!out) throw std::runtime_error("cannot write " + text_);
}

} // namespace httpp
