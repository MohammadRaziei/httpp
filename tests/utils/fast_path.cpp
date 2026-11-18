// The inline fast path and the lazy macros of httpp/logging.hpp (ADR-0090). Links only httpp::utils.
#include <httpp/logging.hpp>

#include <cstdio>
#include <iomanip>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace lg = httpp::logging;

static int g_failed = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                                \
        }                                                                              \
    } while (0)

static int g_calls = 0;
static int expensive() { return ++g_calls; }

// An operator<< that logs on another logger while its line is being built.
struct noisy {
    lg::logger* other;
};
std::ostream& operator<<(std::ostream& os, const noisy& n) {
    HTTPP_LOG_TO(*n.other, info) << "inner";
    return os << "noisy";
}

int main() {
    std::vector<std::string> seen;
    auto& root = lg::get_logger("fp");
    root.set_propagate(false);
    root.set_level(lg::level::info);
    root.add_handler(lg::callback_handler([&seen](const lg::record& r) { seen.emplace_back(r.message); }));
    auto& child = lg::get_logger("fp.child");  // notset: inherits "fp"

    // 1. lazy: the right side is not evaluated when the level is disabled
    HTTPP_LOG_TO(child, debug) << "x=" << expensive();
    HTTPP_LOGF_TO(child, debug, "x=%d", expensive());
    CHECK(g_calls == 0 && seen.empty());
    HTTPP_LOG_TO(child, info) << "x=" << expensive();
    HTTPP_LOGF_TO(child, info, "y=%d", expensive());
    CHECK(g_calls == 2);
    CHECK(seen.size() == 2 && seen[0] == "x=1" && seen[1] == "y=2");

    // 2. the cache follows set_level on this logger and on a parent
    seen.clear();
    CHECK(!child.enabled(lg::level::debug));
    root.set_level(lg::level::debug);
    CHECK(child.enabled(lg::level::debug));
    child.set_level(lg::level::error);
    CHECK(!child.enabled(lg::level::warning) && child.enabled(lg::level::error));
    child.set_level(lg::level::notset);
    root.set_level(lg::level::critical);
    CHECK(!child.enabled(lg::level::error) && child.enabled(lg::level::critical));
    CHECK(child.enabled(lg::level::info) == child.is_enabled_for(lg::level::info));
    root.set_level(lg::level::debug);

    // 3. manipulators, and the formatting state does not leak into the next line
    HTTPP_LOG_TO(child, info) << std::hex << 255 << std::setw(4) << std::setfill('0') << 7;
    HTTPP_LOG_TO(child, info) << 255 << ' ' << 1.5;
    CHECK(seen.size() == 2 && seen[0] == "ff0007" && seen[1] == "255 1.5");
    seen.clear();
    HTTPP_LOG_TO(child, info) << "a" << std::endl;
    CHECK(seen.size() == 1 && seen[0] == "a\n");

    // 4. a line started while another one of this thread is being built
    seen.clear();
    auto& other = lg::get_logger("fp.other");
    HTTPP_LOG_TO(child, info) << "outer " << noisy{&other} << " end";
    CHECK(seen.size() == 2 && seen[0] == "inner" && seen[1] == "outer noisy end");

    // 5. one statement: no dangling else
    seen.clear();
    int other_branch = 0;
    if (g_calls > 100)
        HTTPP_LOG_TO(child, info) << "never";
    else
        other_branch = 1;
    CHECK(other_branch == 1 && seen.empty());
    if (g_calls < 100)
        HTTPP_LOGF_TO(child, info, "%s", "yes");
    else
        other_branch = 2;
    CHECK(other_branch == 1 && seen.size() == 1 && seen[0] == "yes");

    // 6. the logger expression is evaluated once
    seen.clear();
    int lookups = 0;
    auto pick = [&]() -> lg::logger& { ++lookups; return child; };
    HTTPP_LOG_TO(pick(), info) << "once";
    CHECK(lookups == 1 && seen.size() == 1);

    // 7. threads: every enabled line arrives once, a disabled one never
    std::atomic<int> count{0};
    auto& tl = lg::get_logger("fp.threads");
    tl.set_propagate(false);
    tl.set_level(lg::level::info);
    tl.add_handler(lg::callback_handler([&count](const lg::record&) { ++count; }));
    std::vector<std::thread> pool;
    for (int t = 0; t < 4; ++t)
        pool.emplace_back([&tl, t] {
            for (int i = 0; i < 2000; ++i) {
                HTTPP_LOG_TO(tl, info) << "t" << t << " i" << i;
                HTTPP_LOG_TO(tl, debug) << "off " << i;
            }
        });
    for (auto& th : pool) th.join();
    CHECK(count == 8000);

    if (g_failed) return 1;
    std::puts("fast_path: ok");
    return 0;
}
