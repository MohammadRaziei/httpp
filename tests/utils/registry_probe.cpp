// A shared library that links only httpp::utils and logs through the logger "x.y".
#include <httpp/logging.hpp>

#if defined(_WIN32)
#  define PROBE_API extern "C" __declspec(dllexport)
#else
#  define PROBE_API extern "C" __attribute__((visibility("default")))
#endif

PROBE_API void utils_probe_log(const char* name, int levelno, const char* message) {
    httpp::logging::get_logger(name).log_from(static_cast<httpp::logging::level>(levelno), "utils_probe_log", message);
}
