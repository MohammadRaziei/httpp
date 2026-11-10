#include <string>
#include <type_traits>
#include <utility>

#include "internal/logging_os.hpp"
#include "utest/utest.h"

// The handlers that write to the log of the operating system (stb/logging_os.hpp). What each system
// does with a record cannot be read back from a test (journald, logcat, the Event Log and the
// unified log are all somewhere else), so the tests check the part that is ours, the mapping of
// levels to severities, and that each handler of the platform can be built, fed every level, moved
// and destroyed without an error. Every test uses its own logger name and removes its handler.

namespace {

namespace lg = httpp::logging;

// Feeds one record of each level to `log`.
void log_every_level(lg::logger& log) {
    log.debug("httpp test: debug");
    log.info("httpp test: info");
    log.warning("httpp test: warning");
    log.error("httpp test: error");
    log.critical("httpp test: critical");
    log.log(static_cast<lg::level>(25), "httpp test: level 25");
}

}  // namespace

UTEST(logging_os, a_level_takes_the_severity_at_or_below_it) {
    using lg::level;
    namespace d = lg::detail;
    static_assert(d::severity_rank(level::debug) == d::rank_debug, "debug");
    static_assert(d::severity_rank(level::critical) == d::rank_critical, "critical");

    ASSERT_EQ(int(d::rank_debug), d::severity_rank(level::notset));
    ASSERT_EQ(int(d::rank_debug), d::severity_rank(level::debug));
    ASSERT_EQ(int(d::rank_debug), d::severity_rank(static_cast<level>(19)));
    ASSERT_EQ(int(d::rank_info), d::severity_rank(level::info));
    ASSERT_EQ(int(d::rank_info), d::severity_rank(static_cast<level>(25)));
    ASSERT_EQ(int(d::rank_info), d::severity_rank(static_cast<level>(29)));
    ASSERT_EQ(int(d::rank_warning), d::severity_rank(level::warning));
    ASSERT_EQ(int(d::rank_warning), d::severity_rank(static_cast<level>(39)));
    ASSERT_EQ(int(d::rank_error), d::severity_rank(level::error));
    ASSERT_EQ(int(d::rank_error), d::severity_rank(static_cast<level>(49)));
    ASSERT_EQ(int(d::rank_critical), d::severity_rank(level::critical));
    ASSERT_EQ(int(d::rank_critical), d::severity_rank(static_cast<level>(1000)));
}

UTEST(logging_os, the_macros_say_which_handlers_this_platform_has) {
    // each is 0 or 1, and they agree with the platform
    ASSERT_TRUE(HTTPP_LOG_HAS_SYSLOG == 0 || HTTPP_LOG_HAS_SYSLOG == 1);
    ASSERT_TRUE(HTTPP_LOG_HAS_ANDROID == 0 || HTTPP_LOG_HAS_ANDROID == 1);
    ASSERT_TRUE(HTTPP_LOG_HAS_UNIFIED == 0 || HTTPP_LOG_HAS_UNIFIED == 1);
    ASSERT_EQ(HTTPP_LOG_HAS_DEBUGSTRING, HTTPP_LOG_HAS_EVENTLOG);  // the two Windows ones come together
#if defined(_WIN32)
    ASSERT_EQ(1, HTTPP_LOG_HAS_EVENTLOG);
    ASSERT_EQ(0, HTTPP_LOG_HAS_SYSLOG);
#elif defined(__linux__) && !defined(__ANDROID__)
    ASSERT_EQ(1, HTTPP_LOG_HAS_SYSLOG);
    ASSERT_EQ(0, HTTPP_LOG_HAS_EVENTLOG);
#endif
    ASSERT_EQ(0, HTTPP_LOG_HAS_SYSLOG && HTTPP_LOG_HAS_ANDROID);  // Android's syslog is logcat: one handler, not two
}

#if HTTPP_LOG_HAS_SYSLOG
UTEST(logging_os, a_syslog_handler_takes_every_level_and_can_be_moved) {
    static_assert(std::is_base_of<lg::handler, lg::syslog_handler>::value, "a handler");
    static_assert(std::is_move_constructible<lg::syslog_handler>::value, "add_handler moves it into the logger");
    static_assert(!std::is_copy_constructible<lg::syslog_handler>::value, "one connection to syslog");

    lg::logger& log = lg::get_logger("test.logging_os.syslog");
    log.set_propagate(false);
    log.set_level(lg::level::debug);
    lg::syslog_handler& h = log.add_handler(lg::syslog_handler("httpp-test", lg::syslog_facility::user, false));
    ASSERT_EQ(std::size_t(1), log.handler_count());
    log_every_level(log);
    h.set_level(lg::level::warning);  // a handler level and a formatter work as on any handler
    h.set_formatter(lg::formatter("%(levelname)s %(message)s"));
    log_every_level(log);
    log.remove_handler(h);
    ASSERT_EQ(std::size_t(0), log.handler_count());

    lg::syslog_handler a("httpp-test", lg::syslog_facility::local0);
    lg::syslog_handler b(std::move(a));  // the connection goes with the handler, and is closed once
    b = lg::syslog_handler("httpp-test", lg::syslog_facility::daemon, true);
    log.clear_handlers();
}
#endif

#if HTTPP_LOG_HAS_UNIFIED
UTEST(logging_os, a_unified_log_handler_takes_every_level_and_can_be_moved) {
    static_assert(std::is_base_of<lg::handler, lg::unified_log_handler>::value, "a handler");
    static_assert(std::is_move_constructible<lg::unified_log_handler>::value, "add_handler moves it into the logger");

    lg::logger& log = lg::get_logger("test.logging_os.unified");
    log.set_propagate(false);
    log.set_level(lg::level::debug);
    lg::unified_log_handler& h = log.add_handler(lg::unified_log_handler("httpp-test"));
    ASSERT_EQ(std::size_t(1), log.handler_count());
    log_every_level(log);  // the category is the logger's name
    log.remove_handler(h);
    ASSERT_EQ(std::size_t(0), log.handler_count());

    lg::unified_log_handler a("httpp-test");
    lg::unified_log_handler b(std::move(a));
    b = lg::unified_log_handler("httpp-test-2");
    log.clear_handlers();
}
#endif

#if HTTPP_LOG_HAS_ANDROID
UTEST(logging_os, an_android_handler_takes_every_level) {
    lg::logger& log = lg::get_logger("test.logging_os.android");
    log.set_propagate(false);
    log.set_level(lg::level::debug);
    lg::android_handler& h = log.add_handler(lg::android_handler());  // the tag is the logger's name
    log_every_level(log);
    log.remove_handler(h);
    lg::android_handler& fixed = log.add_handler(lg::android_handler("httpp-test"));
    log_every_level(log);
    log.remove_handler(fixed);
    ASSERT_EQ(std::size_t(0), log.handler_count());
}
#endif

#if HTTPP_LOG_HAS_DEBUGSTRING
UTEST(logging_os, a_debug_string_handler_takes_every_level_with_nobody_listening) {
    lg::logger& log = lg::get_logger("test.logging_os.debugstring");
    log.set_propagate(false);
    log.set_level(lg::level::debug);
    lg::debug_string_handler& h = log.add_handler(lg::debug_string_handler());
    log_every_level(log);
    log.info("httpp test: UTF-8 \u0633\u0644\u0627\u0645");  // converted to UTF-16 on the way
    log.remove_handler(h);
    ASSERT_EQ(std::size_t(0), log.handler_count());
}
#endif

#if HTTPP_LOG_HAS_EVENTLOG
UTEST(logging_os, an_event_log_handler_registers_its_source_and_can_be_moved) {
    // Nothing is written to the Event Log: a test run must not leave entries in the machine's log.
    static_assert(std::is_base_of<lg::handler, lg::event_log_handler>::value, "a handler");
    static_assert(std::is_move_constructible<lg::event_log_handler>::value, "add_handler moves it into the logger");
    ASSERT_TRUE(std::is_move_assignable<lg::event_log_handler>::value);  // the `b = ...` below

    lg::event_log_handler a("httpp-test");
    lg::event_log_handler b(std::move(a));  // the registration goes with the handler, and is released once
    b = lg::event_log_handler("httpp-test-2");
}
#endif

// The public switch (httpp/log.hpp): the platform's native log, and the others refused when absent.
#include "httpp/log.hpp"

UTEST(logging_os, set_log_system_switches_to_the_native_log_and_back) {
#if HTTPP_LOG_HAS_SYSLOG || HTTPP_LOG_HAS_ANDROID || HTTPP_LOG_HAS_UNIFIED || HTTPP_LOG_HAS_DEBUGSTRING
    ASSERT_TRUE(httpp::set_log_system(httpp::log_system::native, "httpp-test"));
    ASSERT_TRUE(httpp::set_log_system(httpp::log_system::native, "httpp-test-2")); // again: the newest name wins
    httpp::set_log_level(httpp::log_level::error);
    httpp::set_log_level(httpp::log_level::warning);
#endif
    ASSERT_TRUE(httpp::set_log_file(""));  // back to stderr
}

UTEST(logging_os, a_system_this_platform_does_not_have_is_refused_and_the_output_is_kept) {
#if !HTTPP_LOG_HAS_ANDROID
    ASSERT_FALSE(httpp::set_log_system(httpp::log_system::android));
#endif
#if !HTTPP_LOG_HAS_EVENTLOG
    ASSERT_FALSE(httpp::set_log_system(httpp::log_system::event_log));
#endif
#if !HTTPP_LOG_HAS_UNIFIED
    ASSERT_FALSE(httpp::set_log_system(httpp::log_system::unified));
#endif
}
