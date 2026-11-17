// Links httpp::utils only: logging, sformat and path work without libhttpp (the HTTP stack).
#include <httpp/logging.hpp>
#include <httpp/path.hpp>
#include <httpp/progress.hpp>

#include <cstdio>
#include <string>
#include <vector>

int main() {
    namespace lg = httpp::logging;
    std::vector<std::string> seen;
    auto& log = lg::get_logger("consumer.test");
    log.set_propagate(false);
    log.set_level(lg::level::debug);
    log.add_handler(lg::callback_handler([&seen](const lg::record& r) { seen.emplace_back(r.message); }));

    log.info("hello %d", 42);
    log.debug("%s", lg::sformat("%05.1f", 3.14159).c_str());
    log.log(lg::level::trace, "not shown");  // below the logger's level

    if (seen.size() != 2 || seen[0] != "hello 42" || seen[1] != "003.1") {
        std::fprintf(stderr, "unexpected records (%zu)\n", seen.size());
        return 1;
    }

    const httpp::path p = httpp::path("a") / "b";
    if (p.str().empty()) return 2;

    httpp::set_log_level(lg::level::off);  // the facade of the library's own log
    return 0;
}
