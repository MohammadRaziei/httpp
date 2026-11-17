// The executable installs a handler on the logger "x"; the probe library logs on "x.y". The record must arrive: both modules
// use the one registry of libhttpp_utils.
#include <httpp/logging.hpp>

#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#  define PROBE_API extern "C" __declspec(dllimport)
#else
#  define PROBE_API extern "C"
#endif
PROBE_API void utils_probe_log(const char* name, int levelno, const char* message);

int main() {
    namespace lg = httpp::logging;
    std::vector<std::string> seen;
    auto& root = lg::get_logger("x");
    root.set_propagate(false);
    root.set_level(lg::level::info);
    root.add_handler(lg::callback_handler([&seen](const lg::record& r) { seen.push_back(std::string(r.name) + ":" + std::string(r.message)); }));

    utils_probe_log("x.y", 20, "from the probe");
    utils_probe_log("x.y", 10, "below the level");

    if (seen.size() != 1 || seen[0] != "x.y:from the probe") {
        std::fprintf(stderr, "records lost: %zu received (two registries?)\n", seen.size());
        return 1;
    }
    return 0;
}
