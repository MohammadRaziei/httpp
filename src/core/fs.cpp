#include "internal/fs.hpp"

#include <cerrno>
#include <cstdlib>

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

namespace httpp::fs {

// ---- text helpers --------------------------------------------------------------------------------------------

std::string filename(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    const bool dir_ends = dir.back() == '/' || dir.back() == '\\';
    const bool name_starts = name.front() == '/' || name.front() == '\\';
    if (dir_ends && name_starts) return dir + name.substr(1);
    return dir_ends || name_starts ? dir + name : dir + "/" + name;
}

bool is_inside(const std::string& path, const std::string& root) {
    std::size_t n = root.size();
    while (n > 0 && root[n - 1] == '/') --n; // "/data/" means "/data"; "/" and "C:/" keep "" and "C:"
    if (path.size() < n || path.compare(0, n, root, 0, n) != 0) return false;
    return path.size() == n || path[n] == '/'; // not "/data2" for the root "/data"
}

#ifdef _WIN32

// ---- Windows (wide API; written without a Windows machine to run it on) ---------------------------------------

std::wstring native(const std::string& utf8) {
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

namespace {

long long unix_seconds(const FILETIME& ft, long& nsec) {
    const unsigned long long ticks = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; // 100 ns since 1601
    const unsigned long long since_unix = ticks >= 116444736000000000ULL ? ticks - 116444736000000000ULL : 0;
    nsec = static_cast<long>((since_unix % 10000000ULL) * 100);
    return static_cast<long long>(since_unix / 10000000ULL);
}

std::string to_slashes(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

} // namespace

info stat(const std::string& path) {
    info out;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(native(path).c_str(), GetFileExInfoStandard, &data)) return out;
    out.type = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? kind::directory : kind::file;
    out.size = static_cast<long long>((static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
    out.mtime = unix_seconds(data.ftLastWriteTime, out.mtime_nsec);
    return out;
}

info lstat(const std::string& path) { return stat(path); }

std::string real(const std::string& path) {
    const HANDLE h = CreateFileW(native(path.empty() ? "." : path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::wstring buf(512, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h, &buf[0], static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
    if (n >= buf.size()) {
        buf.assign(n + 1, L'\0');
        n = GetFinalPathNameByHandleW(h, &buf[0], static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED);
    }
    CloseHandle(h);
    if (n == 0 || n >= buf.size()) return "";
    buf.resize(n);
    if (buf.compare(0, 8, L"\\\\?\\UNC\\") == 0) buf = L"\\\\" + buf.substr(8); // \\?\UNC\server\share -> \\server\share
    else if (buf.compare(0, 4, L"\\\\?\\") == 0) buf = buf.substr(4);           // \\?\C:\x -> C:\x
    return to_slashes(narrow(buf));
}

bool is_root(const std::string& r) { return r.size() == 3 && r[1] == ':' && r[2] == '/'; }

bool list(const std::string& dir, std::vector<std::string>& names) {
    WIN32_FIND_DATAW data;
    const HANDLE h = FindFirstFileW(native(join(dir, "*")).c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        const std::wstring name = data.cFileName;
        if (name != L"." && name != L"..") names.push_back(narrow(name));
    } while (FindNextFileW(h, &data));
    FindClose(h);
    return true;
}

made make_dir(const std::string& path) {
    if (CreateDirectoryW(native(path).c_str(), nullptr)) return made::created;
    return GetLastError() == ERROR_ALREADY_EXISTS ? made::existed : made::failed;
}

bool remove_dir(const std::string& path) { return RemoveDirectoryW(native(path).c_str()) != 0; }
bool remove_file(const std::string& path) { return DeleteFileW(native(path).c_str()) != 0; }

bool replace(const std::string& from, const std::string& to) {
    return MoveFileExW(native(from).c_str(), native(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool is_executable(const std::string& path) { return stat(path).type == kind::file; }

#else

// ---- POSIX ---------------------------------------------------------------------------------------------------

namespace {

info from_stat(const struct stat& st) {
    info out;
    if (S_ISREG(st.st_mode)) out.type = kind::file;
    else if (S_ISDIR(st.st_mode)) out.type = kind::directory;
    else out.type = kind::other;
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

info stat(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 ? from_stat(st) : info{};
}

info lstat(const std::string& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return info{};
    info out = from_stat(st);
    out.link = S_ISLNK(st.st_mode);
    if (out.link) out.type = kind::other; // what the link is, is stat()'s answer
    return out;
}

std::string real(const std::string& path) {
    char* r = ::realpath(path.empty() ? "." : path.c_str(), nullptr);
    if (!r) return "";
    std::string out(r);
    std::free(r);
    return out;
}

bool is_root(const std::string& r) { return r == "/"; }

bool list(const std::string& dir, std::vector<std::string>& names) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return false;
    while (const dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    ::closedir(d);
    return true;
}

made make_dir(const std::string& path) {
    if (::mkdir(path.c_str(), 0777) == 0) return made::created;
    return errno == EEXIST ? made::existed : made::failed;
}

bool remove_dir(const std::string& path) { return ::rmdir(path.c_str()) == 0; }
bool remove_file(const std::string& path) { return ::unlink(path.c_str()) == 0; }
bool replace(const std::string& from, const std::string& to) { return ::rename(from.c_str(), to.c_str()) == 0; }

bool is_executable(const std::string& path) { return stat(path).type == kind::file && ::access(path.c_str(), X_OK) == 0; }

#endif

} // namespace httpp::fs
