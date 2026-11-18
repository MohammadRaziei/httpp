// Fails at load time, with a clear message, when libhttpp meets a libhttpp_utils of another ABI (ADR-0086): two wheels that share
// libhttpp_utils are built apart and can end up with files that do not match.
#include "httpp/logging.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
struct utils_abi_check {
    utils_abi_check() {
        if (!httpp::utils_abi_compatible()) {
            std::fprintf(stderr,
                         "httpp: libhttpp was built for libhttpp_utils ABI %d but the loaded libhttpp_utils is ABI %d "
                         "(two different httpp versions in one process?)\n",
                         HTTPP_UTILS_ABI_VERSION, httpp::utils_abi_version());
            std::abort();
        }
    }
} g_utils_abi_check;
}  // namespace
