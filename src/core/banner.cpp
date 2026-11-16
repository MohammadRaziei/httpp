#include "httpp/banner.hpp"

#include "httpp.h"  // HTTPP_VERSION_*

#ifdef _WIN32
#  include <io.h>
#else
#  include <unistd.h>
#endif

namespace httpp {

namespace {
#define HTTPP_BANNER_STR2(x) #x
#define HTTPP_BANNER_STR(x) HTTPP_BANNER_STR2(x)
constexpr const char* k_version = HTTPP_BANNER_STR(HTTPP_VERSION_MAJOR) "." HTTPP_BANNER_STR(HTTPP_VERSION_MINOR) "." HTTPP_BANNER_STR(HTTPP_VERSION_PATCH);

bool is_terminal(std::FILE* f) {
#ifdef _WIN32
    return f != nullptr && _isatty(_fileno(f)) != 0;
#else
    return f != nullptr && ::isatty(::fileno(f)) != 0;
#endif
}
}  // namespace

std::string banner() {
    return std::string(" _   _   _\n"
                       "| |_| |_| |_ _ __ _ __\n"
                       "| ' \\  _|  _| '_ \\ '_ \\\n"
                       "|_||_\\__|\\__| .__/ .__/\n"
                       "            |_|  |_|\n"
                       "  v") + k_version + "\n\n";
}

bool print_banner(std::FILE* out) {
    if (!is_terminal(out)) return false;
    const std::string text = banner();
    return std::fwrite(text.data(), 1, text.size(), out) == text.size();
}

}  // namespace httpp
