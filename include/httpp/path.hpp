#pragma once

// A file system path, shaped like Python's pathlib.Path (ADR-0054). The Python layer takes a pathlib.Path (or a
// str) wherever C++ takes an httpp::path, so the two sides read the same:
//
//   httpp::path dir = "downloads";
//   httpp::download(url, dir / "f.zip").run();
//   if ((dir / "f.zip").exists()) { auto text = (dir / "notes.txt").read_text(); }
//
// A path is a UTF-8 string with '/' as the separator (a '\' is turned into '/' on Windows). It is cleaned on
// construction like pathlib does it: repeated and trailing separators go. Nothing here throws except
// read_text/write_text, and nothing touches the disk except the members marked "disk".
// It is also the file system of httpp (ADR-0054, merged into path by ADR-0054): httpp uses no std::filesystem, which the
// macOS 10.13 wheels do not have. On Windows every call goes through the wide API, so a non-ASCII name works.
// std::string and const char* convert to it implicitly, so a function that takes `const path&` still accepts
// the strings it took before. Not covered: iterating the parts, relative_to, glob, permissions, copying, recursive operations, temporary names.

#include "httpp/export.hpp"

#include <fstream>
#include <ios>
#include <string>
#include <utility>
#include <vector>

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
    HTTPP_UTILS_API std::wstring native() const;
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
    HTTPP_UTILS_API static path cwd();                // pathlib: Path.cwd()
    HTTPP_UTILS_API path absolute() const;            // cwd() / this, unless it is absolute already
    // "file:///dir/f.txt", percent-encoded, made absolute first (pathlib: as_uri, which refuses a relative path).
    HTTPP_UTILS_API std::string as_uri() const;

    // "/" or "C:/": a path that has no parent (a resolve() result).
    bool is_root() const { return text_ == "/" || (text_.size() == 3 && text_[1] == ':' && text_[2] == '/'); }
    // Is this path `root` itself or something below it? Whole parts: "/data2" is not inside "/data". Text only: resolve()
    // both first when links matter (pathlib: is_relative_to).
    bool is_relative_to(const path& root) const {
        std::size_t n = root.text_.size();
        while (n > 0 && root.text_[n - 1] == '/') --n; // "/data/" means "/data"; "/" and "C:/" keep "" and "C:"
        if (text_.size() < n || text_.compare(0, n, root.text_, 0, n) != 0) return false;
        return text_.size() == n || text_[n] == '/';
    }

    // --- disk (follow symbolic links) ---
    enum class kind { missing, file, directory, other }; // other: a socket, a pipe, a device
    struct status {
        kind type = kind::missing;
        long long size = 0;   // bytes (a file)
        long long mtime = 0;  // seconds since the epoch, of the last write
        long mtime_nsec = 0;  // the sub-second part where the platform has one, else 0
        bool link = false;    // lstat() only: the path itself is a symbolic link
    };
    HTTPP_UTILS_API status stat() const;  // follows symbolic links
    HTTPP_UTILS_API status lstat() const; // does not (on Windows both are the same)
    // The absolute path with every link resolved, "" when it does not exist (pathlib: resolve(strict=True) without the error).
    HTTPP_UTILS_API path resolve() const;
    // The entries of a directory without "." and "..", in no particular order. False when it cannot be read.
    HTTPP_UTILS_API bool iterdir(std::vector<path>& entries) const;
    // A regular file this process may run (Windows: a file that exists).
    HTTPP_UTILS_API bool is_executable() const;
    HTTPP_UTILS_API bool exists() const;
    HTTPP_UTILS_API bool is_file() const;
    HTTPP_UTILS_API bool is_dir() const;
    HTTPP_UTILS_API long long size() const;           // bytes of a file, -1 when it is not one
    // One level. True only when THIS call created the directory: creating one is atomic, so the winner owns it (the
    // download lock stands on that). False: it was there already (is_dir() tells) or it could not be made.
    HTTPP_UTILS_API bool mkdir() const;
    // pathlib's mkdir(parents=True, exist_ok=True): every missing level. True when the directory exists afterwards.
    bool mkdirs() const {
        if (empty()) return false;
        if (is_dir()) return true;
        if (!is_root()) parent().mkdirs(); // "a" has the parent ".", which is there; "/" and "C:/" are roots
        mkdir();                           // may lose a race with another process: is_dir() tells
        return is_dir();
    }
    HTTPP_UTILS_API bool rmdir() const;                // an empty directory
    HTTPP_UTILS_API bool unlink() const;               // a file
    // shutil.rmtree, and it takes a file too: a file, a link, or a directory with everything in it, every level. A symbolic
    // link is removed and never followed, so nothing outside this path is touched (Windows: a directory link or junction is
    // removed as a link, and a read-only attribute is cleared first). True when this is gone afterwards, also when it was
    // never there.
    HTTPP_UTILS_API bool rmtree() const;
    // Moves this over `to`, replacing it. std::rename does that on POSIX only (on Windows it fails when `to` exists);
    // this is atomic on both.
    HTTPP_UTILS_API bool rename(const path& to) const;

    // pathlib's open(): the stream is closed when it goes out of scope. Check it with `if (!f)`.
    std::fstream open(std::ios::openmode mode = std::ios::in | std::ios::binary) const {
        std::fstream f;
        f.open(native(), mode);
        return f;
    }
    // The whole file, or with `start` and `end` the bytes [start, end) of it (cut at the end of the file; empty when
    // start is at or past it or end <= start); throws std::runtime_error when it cannot be read / written. For a short
    // line in a file that may be large (a checksum): read_text(0, 1025).
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    HTTPP_UTILS_API std::string read_text(std::size_t start = 0, std::size_t end = npos) const;
    HTTPP_UTILS_API void write_text(const std::string& data) const;
    // write_text() as a protocol other readers and writers can rely on: the data goes to a file of its own beside this one
    // ("<name>.tmp.<random>.<n>", on the same disk), and rename() then puts it in place, so at every moment the path holds
    // the whole old text or the whole new one, for another thread and for another process alike; a crash or a full disk
    // leaves the old file as it was. Throws std::runtime_error (and leaves no temporary file) when it cannot. Two of
    // these at once do not corrupt anything: the last rename wins. It does not make read-modify-write safe (take a lock
    // for that, as download does), and it does not fsync.
    HTTPP_UTILS_API void write_text_atomic(const std::string& data) const;

private:
    std::size_t name_start() const {
        const auto slash = text_.rfind('/');
        return slash == std::string::npos ? 0 : slash + 1;
    }
    path parent_for_join() const { return text_.find('/') == std::string::npos ? path() : parent(); }
    static HTTPP_UTILS_API std::string clean(std::string text);
    std::string text_;
};

} // namespace httpp

// The directory of the source file it is written in: Python's Path(__file__).parent, for data that lies next to the code
// (a test fixture, a template). A macro because __FILE__ must expand where the macro is used. It is not the working directory
// of the process: that is path::cwd().
#define HTTPP_PATH_CWD (::httpp::path(__FILE__).parent())
