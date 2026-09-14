#pragma once

// Internal helper, NOT installed and NEVER included from a public
// httpp/*.hpp header. Every src/core/*.cpp file that talks to cpp-httplib
// includes THIS instead of <httplib.h> directly, so:
//   - the win32/winsock ordering fix and the mbedtls-support macro are
//     defined in exactly one place (not repeated per file), and
//   - httpp_core is compiled with CPPHTTPLIB_MBEDTLS_SUPPORT (see the
//     matching target_link_libraries/target_compile_definitions for
//     httpp_core in the root CMakeLists.txt), so httplib::Client
//     automatically uses httplib::SSLClient for "https://" URLs.

#ifdef _WIN32
// Must come before <httplib.h> (which pulls in <winsock2.h>): without this,
// <windows.h> drags in the legacy <winsock.h> first and the two conflict.
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#endif

#include <httplib.h>

#include <string>

namespace httpp::detail {

// httplib::Client's "scheme://host:port" constructor is what actually picks
// SSLClient vs. plain Client internally based on scheme — the (host, port)
// constructor always builds a plain, non-TLS client regardless of what
// port you pass. Every httpp call site that builds a client from a parsed
// httpp::url must go through this (not the (host, port) constructor) or
// HTTPS silently doesn't happen.
inline std::string scheme_host_port(const std::string& scheme, const std::string& host, int port) {
    return scheme + "://" + host + ":" + std::to_string(port);
}

} // namespace httpp::detail
