#include "httpp/path.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <iterator>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// ---- Windows (wide API; written without a Windows machine to run it on) ---------------------------------------

namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &wide[0], n);
    return wide;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &out[0], n, nullptr, nullptr);
    return out;
}

long long unix_seconds(const FILETIME& ft, long& nsec) {
    const unsigned long long ticks = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; // 100 ns since 1601
    const unsigned long long since_unix = ticks >= 116444736000000000ULL ? ticks - 116444736000000000ULL : 0;
    nsec = static_cast<long>((since_unix % 10000000ULL) * 100);
    return static_cast<long long>(since_unix / 10000000ULL);
}

} // namespace

std::wstring path::native() const { return widen(text_); }

path::status path::stat() const {
    status out;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(native().c_str(), GetFileExInfoStandard, &data)) return out;
    out.type = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? kind::directory : kind::file;
    out.size = static_cast<long long>((static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
    out.mtime = unix_seconds(data.ftLastWriteTime, out.mtime_nsec);
    return out;
}

path::status path::lstat() const { return stat(); }

path path::cwd() {
    const DWORD n = GetCurrentDirectoryW(0, nullptr);
    if (n == 0) return path();
    std::wstring wide(n, L'\0');
    const DWORD got = GetCurrentDirectoryW(n, &wide[0]);
    if (got == 0 || got >= n) return path();
    wide.resize(got);
    return path(narrow(wide)); // the constructor turns '\' into '/'
}

path path::resolve() const {
    const HANDLE h = CreateFileW(path(text_.empty() ? "." : text_).native().c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return path();
    std::wstring buf(512, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h, &buf[0], static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
    if (n >= buf.size()) {
        buf.assign(n + 1, L'\0');
        n = GetFinalPathNameByHandleW(h, &buf[0], static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
    }
    CloseHandle(h);
    if (n == 0 || n >= buf.size()) return path();
    buf.resize(n);
    if (buf.compare(0, 8, L"\\\\?\\UNC\\") == 0) buf = L"\\\\" + buf.substr(8); // \\?\UNC\server\share -> \\server\share
    else if (buf.compare(0, 4, L"\\\\?\\") == 0) buf = buf.substr(4);           // \\?\C:\x -> C:\x
    return path(narrow(buf));
}

bool path::iterdir(std::vector<path>& entries) const {
    WIN32_FIND_DATAW data;
    const HANDLE h = FindFirstFileW((*this / "*").native().c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        const std::wstring name = data.cFileName;
        if (name != L"." && name != L"..") entries.push_back(*this / narrow(name));
    } while (FindNextFileW(h, &data));
    FindClose(h);
    return true;
}

bool path::mkdir() const { return CreateDirectoryW(native().c_str(), nullptr) != 0; }
bool path::rmdir() const { return RemoveDirectoryW(native().c_str()) != 0; }
bool path::unlink() const { return DeleteFileW(native().c_str()) != 0; }

bool path::rename(const path& to) const {
    return MoveFileExW(native().c_str(), to.native().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool path::is_executable() const { return stat().type == kind::file; }

#else

// ---- POSIX ---------------------------------------------------------------------------------------------------

namespace {

path::status from_stat(const struct stat& st) {
    path::status out;
    if (S_ISREG(st.st_mode)) out.type = path::kind::file;
    else if (S_ISDIR(st.st_mode)) out.type = path::kind::directory;
    else out.type = path::kind::other;
    out.size = static_cast<long long>(st.st_size);
    out.mtime = static_cast<long long>(st.st_mtime);
#if defined(__APPLE__)
    out.mtime_nsec = static_cast<long>(st.st_mtimespec.tv_nsec);
#else
    out.mtime_nsec = static_cast<long>(st.st_mtim.tv_nsec);
#endif
    return out;
}

} // namespace

path::status path::stat() const {
    struct stat st;
    return ::stat(text_.c_str(), &st) == 0 ? from_stat(st) : status{};
}

path::status path::lstat() const {
    struct stat st;
    if (::lstat(text_.c_str(), &st) != 0) return status{};
    status out = from_stat(st);
    out.link = S_ISLNK(st.st_mode);
    if (out.link) out.type = kind::other; // what the link is, is stat()'s answer
    return out;
}

path path::cwd() {
    std::string buf(4096, '\0');
    while (!::getcwd(&buf[0], buf.size())) {
        if (errno != ERANGE || buf.size() > (1u << 20)) return path();
        buf.resize(buf.size() * 2);
    }
    buf.resize(buf.find('\0'));
    return path(buf);
}

path path::resolve() const {
    char* r = ::realpath(text_.empty() ? "." : text_.c_str(), nullptr);
    if (!r) return path();
    std::string out(r);
    std::free(r);
    return path(out);
}

bool path::iterdir(std::vector<path>& entries) const {
    DIR* d = ::opendir(text_.c_str());
    if (!d) return false;
    while (const dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") entries.push_back(*this / name);
    }
    ::closedir(d);
    return true;
}

bool path::mkdir() const { return ::mkdir(text_.c_str(), 0777) == 0; }
bool path::rmdir() const { return ::rmdir(text_.c_str()) == 0; }
bool path::unlink() const { return ::unlink(text_.c_str()) == 0; }
bool path::rename(const path& to) const { return ::rename(text_.c_str(), to.text_.c_str()) == 0; }

bool path::is_executable() const { return stat().type == kind::file && ::access(text_.c_str(), X_OK) == 0; }

#endif

// ---- the same everywhere ---------------------------------------------------------------------------------------

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

bool path::exists() const { return stat().type != kind::missing; }
bool path::is_file() const { return stat().type == kind::file; }
bool path::is_dir() const { return stat().type == kind::directory; }

long long path::size() const {
    const status i = stat();
    return i.type == kind::file ? i.size : -1;
}

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
