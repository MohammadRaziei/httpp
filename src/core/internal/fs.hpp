#pragma once

// Internal, NOT installed: the file system, once (ADR-0054). httpp does not use std::filesystem: the macOS wheels
// target 10.13, where it is unavailable (it needs 10.15), so a use of it fails the macOS build and nothing else.
// This module is what the library, the CLI and the server use instead, with the same behaviour on every platform.
//
// A path is a UTF-8 std::string. '/' separates its parts (on Windows '\' is accepted on input and results come back
// with '/'). On Windows every call converts to UTF-16 and uses the wide API, so a non-ASCII name works there and
// the ANSI code page is never involved. Calls report failure through their return value and never throw.
//
// Not covered, on purpose (nothing needs them): permissions, copying, recursive operations, temporary names.

#include <string>
#include <vector>

namespace httpp::fs {

enum class kind { missing, file, directory, other }; // other: a socket, a pipe, a device

struct info {
    kind type = kind::missing;
    long long size = 0;       // bytes (a file)
    long long mtime = 0;      // seconds since the epoch, of the last write
    long mtime_nsec = 0;      // the sub-second part where the platform has one, else 0
    bool link = false;        // lstat() only: the path itself is a symbolic link
};

// What a path is. stat() follows symbolic links; lstat() does not (on Windows both are the same).
info stat(const std::string& path);
info lstat(const std::string& path);

// The absolute path with every link resolved ("" when it does not exist), as the same kind of string stat takes.
std::string real(const std::string& path);

// `path` and `root` both come from real(): is `path` the root itself or something below it? Never true for a
// sibling that merely starts with the same letters ("/data2" is not inside "/data").
bool is_inside(const std::string& path, const std::string& root);

// "/" or "C:/": a real() result that has no parent.
bool is_root(const std::string& real_path);

// The names in a directory without "." and "..", in no particular order. False when it cannot be read.
bool list(const std::string& dir, std::vector<std::string>& names);

enum class made { created, existed, failed };
// One level only. `existed` (a directory or a file is already there) is told apart from `failed` because creating
// a directory is atomic, which is what the cross-process download lock stands on.
made make_dir(const std::string& path);

bool remove_dir(const std::string& path);  // an empty directory
bool remove_file(const std::string& path);
// Moves `from` over `to`, replacing it. std::rename does that on POSIX only (on Windows it fails when `to`
// exists); this is atomic on both.
bool replace(const std::string& from, const std::string& to);

// A regular file this process may run (Windows: a file that exists).
bool is_executable(const std::string& path);

// Text helpers (no disk access).
std::string filename(const std::string& path);             // after the last '/' or '\'
std::string join(const std::string& dir, const std::string& name); // "a" + "b" -> "a/b", no doubled '/'

// What the standard file streams take: the path itself, or on Windows its UTF-16 form (a std::wstring).
//   std::ifstream in(httpp::fs::native(path), std::ios::binary);
#ifdef _WIN32
std::wstring native(const std::string& utf8);
std::string narrow(const std::wstring& wide);
#else
inline const std::string& native(const std::string& utf8) { return utf8; }
#endif

} // namespace httpp::fs
