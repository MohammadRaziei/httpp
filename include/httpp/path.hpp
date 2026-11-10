#pragma once

// A file system path, shaped like Python's pathlib.Path (ADR-0055). The Python layer takes a pathlib.Path (or a
// str) wherever C++ takes an httpp::path, so the two sides read the same:
//
//   httpp::path dir = "downloads";
//   httpp::download(url, dir / "f.zip").run();
//   if ((dir / "f.zip").exists()) { auto text = (dir / "notes.txt").read_text(); }
//
// A path is a UTF-8 string with '/' as the separator (a '\' is turned into '/' on Windows). It is cleaned on
// construction like pathlib does it: repeated and trailing separators go. Nothing here throws except
// read_text/write_text, and nothing touches the disk except the members marked "disk".
// std::string and const char* convert to it implicitly, so a function that takes `const path&` still accepts
// the strings it took before. Not covered: iterating the parts, relative_to, glob, resolve.

#include "httpp/export.hpp"

#include <fstream>
#include <ios>
#include <string>
#include <utility>

namespace httpp {

class path {
public:
    path() = default;
    path(std::string text) : text_(clean(std::move(text))) {}
    path(const char* text) : text_(clean(text ? text : "")) {}

    const std::string& str() const { return text_; }
    const char* c_str() const { return text_.c_str(); }
    bool empty() const { return text_.empty(); }
    // The path as the standard file streams take it: the string itself, on Windows its UTF-16 form.
#ifdef _WIN32
    HTTPP_API std::wstring native() const;
#else
    const std::string& native() const { return text_; }
#endif

    // --- text only (pathlib: name, stem, suffix, parent, with_name, with_suffix, is_absolute, /) ---
    path name() const { return path(text_.substr(name_start())); }
    path suffix() const {
        const std::string n = text_.substr(name_start());
        const auto dot = n.rfind('.');
        return path(dot == std::string::npos || dot == 0 || dot + 1 == n.size() ? std::string() : n.substr(dot));
    }
    path stem() const {
        const std::string n = text_.substr(name_start());
        const auto dot = n.rfind('.');
        return path(dot == std::string::npos || dot == 0 || dot + 1 == n.size() ? n : n.substr(0, dot));
    }
    // "a/b" -> "a", "a" -> ".", "/a" -> "/", "/" -> "/", "" -> "".
    path parent() const {
        if (text_.empty()) return path();
        const auto slash = text_.rfind('/');
        if (slash == std::string::npos) return path(".");
        if (slash == 0) return path("/");
        if (slash == 2 && text_[1] == ':') return path(text_.substr(0, 3)); // "C:/a" -> "C:/"
        return path(text_.substr(0, slash));
    }
    path with_name(const std::string& new_name) const { return parent_for_join() / new_name; }
    path with_suffix(const std::string& new_suffix) const {
        return path(text_.substr(0, text_.size() - suffix().str().size()) + new_suffix);
    }
    bool is_absolute() const {
        return (!text_.empty() && text_[0] == '/') || (text_.size() >= 3 && text_[1] == ':' && text_[2] == '/');
    }
    path operator/(const path& other) const {
        if (other.empty()) return *this;
        if (text_.empty() || other.is_absolute()) return other;
        return path(text_ + (text_.back() == '/' ? "" : "/") + other.text_);
    }
    path& operator/=(const path& other) { return *this = *this / other; }
    friend bool operator==(const path& a, const path& b) { return a.text_ == b.text_; }
    friend bool operator!=(const path& a, const path& b) { return a.text_ != b.text_; }
    friend bool operator<(const path& a, const path& b) { return a.text_ < b.text_; }

    // The text with '/' separators (pathlib: as_posix). A path already is that; str() is the same string.
    const std::string& as_posix() const { return text_; }

    // --- needs the current directory ---
    HTTPP_API static path cwd();                // pathlib: Path.cwd()
    HTTPP_API path absolute() const;            // cwd() / this, unless it is absolute already
    // "file:///dir/f.txt", percent-encoded, made absolute first (pathlib: as_uri, which refuses a relative path).
    HTTPP_API std::string as_uri() const;

    // --- disk (follow symbolic links) ---
    HTTPP_API bool exists() const;
    HTTPP_API bool is_file() const;
    HTTPP_API bool is_dir() const;
    HTTPP_API long long size() const;           // bytes of a file, -1 when it is not one
    HTTPP_API bool mkdir() const;               // one level; true when the directory is there afterwards
    HTTPP_API bool unlink() const;              // a file
    HTTPP_API bool rename(const path& to) const; // moves over `to`, replacing it (atomic)

    // pathlib's open(): the stream is closed when it goes out of scope. Check it with `if (!f)`.
    std::fstream open(std::ios::openmode mode = std::ios::in | std::ios::binary) const {
        std::fstream f;
        f.open(native(), mode);
        return f;
    }
    // The whole file; throws std::runtime_error when it cannot be read / written.
    HTTPP_API std::string read_text() const;
    HTTPP_API void write_text(const std::string& data) const;

private:
    std::size_t name_start() const {
        const auto slash = text_.rfind('/');
        return slash == std::string::npos ? 0 : slash + 1;
    }
    path parent_for_join() const { return text_.find('/') == std::string::npos ? path() : parent(); }
    static HTTPP_API std::string clean(std::string text);
    std::string text_;
};

} // namespace httpp
