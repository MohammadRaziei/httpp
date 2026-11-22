#pragma once

// Internal. UTF-8 <-> UTF-16 for the Windows wide API: the text of the library is UTF-8 everywhere (paths, environment variables), and the
// Windows functions that take or give a string in the code page (the narrow ones) cannot hold every name, so each call goes through the wide
// one. Written without a Windows machine to run it on.

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace httpp::detail {

inline std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &wide[0], n);
    return wide;
}

inline std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &out[0], n, nullptr, nullptr);
    return out;
}

} // namespace httpp::detail

#endif
