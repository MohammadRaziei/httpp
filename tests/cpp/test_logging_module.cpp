#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "utest/utest.h"
#include "httpp/logging.hpp"

// httpp::logging: levels, the logger tree, handlers, the formatter. Every test uses its own logger names
// ("test.logging.<case>") and handlers it adds itself, so tests do not see each other and the root logger
// is left as it was found.

#define ASSERT_SAME(a, b) ASSERT_TRUE(std::string(a) == std::string(b))
#define ASSERT_THROWS(expr, type)                                  \
    do {                                                           \
        bool threw_ = false;                                       \
        try { (void)(expr); } catch (const type&) { threw_ = true; } \
        ASSERT_TRUE(threw_);                                       \
    } while (0)

namespace {

namespace lg = httpp::logging;

struct collector {
    std::vector<std::string> messages;
    std::vector<lg::level> levels;
};

// A handler that appends each record to a collector (through the callback handler).
lg::callback_handler into(collector& c) {
    return lg::callback_handler([&c](const lg::record& r) {
        c.messages.emplace_back(r.message);
        c.levels.push_back(r.levelno);
    });
}

httpp::path tmp(const char* name) { return httpp::path(name); } // relative: the test runs in its build directory

// The file as text: a file_handler writes native line ends (CRLF on Windows, as Python's FileHandler does), and
// reading in text mode gives them back as "\n".
std::string slurp(const httpp::path& p) {
    std::ifstream f(p.native());
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

// A temporary FILE for a stream_handler (tmpfile_s is the form MSVC asks for).
std::FILE* temp_file() {
#if defined(_MSC_VER)
    std::FILE* f = nullptr;
    return tmpfile_s(&f) == 0 ? f : nullptr;
#else
    return std::tmpfile();
#endif
}

lg::record make_record(const char* name, lg::level lv, const char* message, std::chrono::milliseconds since_epoch) {
    return lg::record{{}, name, lv, message, std::chrono::system_clock::time_point(since_epoch), std::this_thread::get_id()};
}

}  // namespace

// ------------------------------------------------------------------------------------ levels

UTEST(httpp_logging_tests, test_level_names_and_numbers_are_pythons) {
    ASSERT_EQ(int(lg::level::debug), 10);
    ASSERT_EQ(int(lg::level::info), 20);
    ASSERT_EQ(int(lg::level::warning), 30);
    ASSERT_EQ(int(lg::level::error), 40);
    ASSERT_EQ(int(lg::level::critical), 50);
    ASSERT_SAME(lg::level_name(lg::level::notset), "NOTSET");
    ASSERT_SAME(lg::level_name(lg::level::warning), "WARNING");
    ASSERT_SAME(lg::level_name(lg::level::critical), "CRITICAL");
    ASSERT_SAME(lg::level_name(lg::level(25)), "Level 25");
}

UTEST(httpp_logging_tests, test_level_from_string) {
    ASSERT_TRUE(lg::level_from_string("debug") == lg::level::debug);
    ASSERT_TRUE(lg::level_from_string("  Info ") == lg::level::info);
    ASSERT_TRUE(lg::level_from_string("WARN") == lg::level::warning);
    ASSERT_TRUE(lg::level_from_string("warning") == lg::level::warning);
    ASSERT_TRUE(lg::level_from_string("fatal") == lg::level::critical);
    ASSERT_TRUE(lg::level_from_string("25") == lg::level(25));
    ASSERT_TRUE(!lg::level_from_string("").has_value());
    ASSERT_TRUE(!lg::level_from_string("loud").has_value());
    ASSERT_TRUE(!lg::level_from_string("-5").has_value());
    ASSERT_TRUE(!lg::level_from_string("12345").has_value());
}

// ------------------------------------------------------------------------- the registry and tree

UTEST(httpp_logging_tests, test_get_logger_returns_the_same_logger_for_a_name) {
    lg::logger& a = lg::get_logger("test.logging.same");
    lg::logger& b = lg::get_logger("test.logging.same");
    ASSERT_TRUE(&a == &b);
    ASSERT_SAME(a.name(), "test.logging.same");
    ASSERT_TRUE(&lg::get_logger() == &lg::get_logger("root"));
    ASSERT_SAME(lg::get_logger().name(), "root");
    ASSERT_TRUE(lg::get_logger().parent() == nullptr);
    ASSERT_TRUE(lg::get_logger().get_level() == lg::level::warning);
}

UTEST(httpp_logging_tests, test_dotted_names_make_a_tree) {
    lg::logger& top = lg::get_logger("test.logging.tree");
    lg::logger& mid = lg::get_logger("test.logging.tree.mid");
    lg::logger& leaf = lg::get_logger("test.logging.tree.mid.leaf");
    ASSERT_TRUE(leaf.parent() == &mid);
    ASSERT_TRUE(mid.parent() == &top);
    ASSERT_TRUE(top.parent() != nullptr);
    // a name with no logger above it hangs from the root
    ASSERT_TRUE(lg::get_logger("test_logging_alone").parent() == &lg::get_logger());
    // httpp's own loggers all sit under "httpp"
    ASSERT_TRUE(lg::get_logger("httpp").parent() == &lg::get_logger());
    ASSERT_TRUE(lg::get_logger("httpp.simd").parent() == &lg::get_logger("httpp"));
}

UTEST(httpp_logging_tests, test_a_logger_made_before_its_parent_is_adopted_by_it) {
    lg::logger& leaf = lg::get_logger("test.logging.late.a.b.c");
    ASSERT_TRUE(leaf.parent() != nullptr);
    lg::logger& deep = lg::get_logger("test.logging.late.a.b");  // closer ancestor, made afterwards
    ASSERT_TRUE(leaf.parent() == &deep);
    lg::logger& far_up = lg::get_logger("test.logging.late.a");
    ASSERT_TRUE(deep.parent() == &far_up);
    ASSERT_TRUE(leaf.parent() == &deep);  // still the nearest one
}

// ---------------------------------------------------------------------------------------- levels

UTEST(httpp_logging_tests, test_effective_level_comes_from_the_nearest_set_ancestor) {
    lg::logger& parent = lg::get_logger("test.logging.eff");
    lg::logger& child = lg::get_logger("test.logging.eff.child");
    ASSERT_TRUE(parent.get_level() == lg::level::notset);
    ASSERT_TRUE(child.effective_level() == lg::level::warning);  // from the root
    parent.set_level(lg::level::info);
    ASSERT_TRUE(child.get_level() == lg::level::notset);
    ASSERT_TRUE(child.effective_level() == lg::level::info);
    ASSERT_TRUE(child.is_enabled_for(lg::level::info));
    ASSERT_TRUE(!child.is_enabled_for(lg::level::debug));
    child.set_level(lg::level::debug);
    ASSERT_TRUE(child.is_enabled_for(lg::level::debug));
    ASSERT_TRUE(parent.effective_level() == lg::level::info);  // the child's level does not go up
    parent.set_level(lg::level::notset);
    child.set_level(lg::level::notset);
}

UTEST(httpp_logging_tests, test_a_disabled_level_builds_nothing_and_an_enabled_one_formats_printf_style) {
    lg::logger& log = lg::get_logger("test.logging.printf");
    collector c;
    auto& h = log.add_handler(into(c));
    log.set_level(lg::level::info);

    log.debug("not %d", 1);
    log.info("loaded %dx%d (%s) %.2f", 640, 480, "photo", 1.5);
    log.warning("%s", "100%");
    log.error("e");
    log.critical("c");
    log.log(lg::level(25), "between %d", 25);
    log.log(lg::level::debug, "also dropped");

    ASSERT_EQ(int(c.messages.size()), 5);
    ASSERT_SAME(c.messages[0], "loaded 640x480 (photo) 1.50");
    ASSERT_SAME(c.messages[1], "100%");
    ASSERT_SAME(c.messages[2], "e");
    ASSERT_SAME(c.messages[3], "c");
    ASSERT_SAME(c.messages[4], "between 25");
    ASSERT_TRUE(c.levels[0] == lg::level::info);
    ASSERT_TRUE(c.levels[4] == lg::level(25));

    log.remove_handler(h);
    log.set_level(lg::level::notset);
}

// ---------------------------------------------------------------------------------------- handlers

UTEST(httpp_logging_tests, test_records_go_up_to_the_parents_handlers_unless_propagate_is_off) {
    lg::logger& parent = lg::get_logger("test.logging.up");
    lg::logger& child = lg::get_logger("test.logging.up.child");
    parent.set_level(lg::level::debug);
    collector on_parent, on_child;
    auto& hp = parent.add_handler(into(on_parent));
    auto& hc = child.add_handler(into(on_child));

    child.info("one");
    ASSERT_EQ(int(on_child.messages.size()), 1);
    ASSERT_EQ(int(on_parent.messages.size()), 1);
    ASSERT_TRUE(child.propagate());

    child.set_propagate(false);
    child.info("two");
    ASSERT_EQ(int(on_child.messages.size()), 2);
    ASSERT_EQ(int(on_parent.messages.size()), 1);

    child.set_propagate(true);
    parent.remove_handler(hp);
    child.remove_handler(hc);
    parent.set_level(lg::level::notset);
}

UTEST(httpp_logging_tests, test_a_handler_has_its_own_level_and_filters) {
    lg::logger& log = lg::get_logger("test.logging.hlevel");
    log.set_level(lg::level::debug);
    collector all, errors, filtered;
    auto& h1 = log.add_handler(into(all));
    auto& h2 = log.add_handler(into(errors));
    h2.set_level(lg::level::error);
    auto& h3 = log.add_handler(into(filtered));
    h3.add_filter([](const lg::record& r) { return r.message.find("keep") != std::string_view::npos; });

    log.info("keep this");
    log.info("drop this");
    log.error("keep the error");

    ASSERT_EQ(int(all.messages.size()), 3);
    ASSERT_EQ(int(errors.messages.size()), 1);
    ASSERT_SAME(errors.messages[0], "keep the error");
    ASSERT_EQ(int(filtered.messages.size()), 2);
    ASSERT_TRUE(h2.get_level() == lg::level::error);

    log.clear_handlers();
    ASSERT_EQ(int(log.handler_count()), 0);
    (void)h1;
    log.set_level(lg::level::notset);
}

UTEST(httpp_logging_tests, test_remove_handler_stops_delivery_and_has_handlers_follows_the_tree) {
    lg::logger& parent = lg::get_logger("test.logging.rm");
    lg::logger& child = lg::get_logger("test.logging.rm.child");
    ASSERT_TRUE(!child.has_handlers());
    collector c;
    auto& h = parent.add_handler(into(c));
    ASSERT_TRUE(child.has_handlers());
    child.set_propagate(false);
    ASSERT_TRUE(!child.has_handlers());
    child.set_propagate(true);

    parent.set_level(lg::level::info);
    child.info("seen");
    parent.remove_handler(h);
    child.info("not seen");
    ASSERT_EQ(int(c.messages.size()), 1);
    ASSERT_TRUE(!child.has_handlers());
    parent.set_level(lg::level::notset);
}

namespace {
struct counting_handler : lg::handler {
    int* count;
    explicit counting_handler(int* c) : count(c) {}
    void emit(const lg::record& r) override {
        ++*count;
        last = format(r);
    }
    std::string last;
};
struct throwing_handler : lg::handler {
    void emit(const lg::record&) override { throw std::runtime_error("handler failed"); }
};
}  // namespace

UTEST(httpp_logging_tests, test_a_custom_handler_uses_its_formatter_and_a_failing_one_breaks_nothing) {
    lg::logger& log = lg::get_logger("test.logging.custom");
    log.set_level(lg::level::debug);
    int count = 0;
    auto& counting = log.add_handler(counting_handler(&count));
    counting.set_formatter(lg::formatter("%(levelname)s|%(name)s|%(message)s"));
    log.add_handler(throwing_handler());
    collector after;
    log.add_handler(into(after));  // added after the failing one: it must still get the record

    log.warning("w %d", 7);  // must not throw
    ASSERT_EQ(count, 1);
    ASSERT_SAME(counting.last, "WARNING|test.logging.custom|w 7");
    ASSERT_EQ(int(after.messages.size()), 1);

    log.clear_handlers();
    log.set_level(lg::level::notset);
}

UTEST(httpp_logging_tests, test_stream_handler_writes_a_line_and_colors_only_when_asked) {
    std::FILE* plain = temp_file();
    std::FILE* colored = temp_file();
    ASSERT_TRUE(plain != nullptr && colored != nullptr);

    lg::logger& log = lg::get_logger("test.logging.stream");
    log.set_level(lg::level::debug);
    log.set_propagate(false);
    auto& h1 = log.add_handler(lg::stream_handler(plain, lg::color_mode::never));
    h1.set_formatter(lg::formatter("%(levelname)s %(message)s"));
    auto& h2 = log.add_handler(lg::stream_handler(colored, lg::color_mode::always));
    h2.set_formatter(lg::formatter("%(message)s"));

    log.warning("careful");
    log.clear_handlers();
    log.set_propagate(true);
    log.set_level(lg::level::notset);

    auto read_all = [](std::FILE* f) {
        std::rewind(f);
        std::string out;
        char buf[256];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        std::fclose(f);
        return out;
    };
    ASSERT_SAME(read_all(plain), "WARNING careful\n");
    ASSERT_SAME(read_all(colored), "\x1b[33mcareful\x1b[0m\n");
}

UTEST(httpp_logging_tests, test_file_handler_appends_truncates_and_reports_an_unopenable_file) {
    const auto path = tmp("httpp_test_logging.log");
    path.unlink();
    lg::logger& log = lg::get_logger("test.logging.file");
    log.set_level(lg::level::info);
    log.set_propagate(false);

    {
        auto& h = log.add_handler(lg::file_handler(path));
        h.set_formatter(lg::formatter("%(levelname)s:%(message)s"));
        log.info("first");
        log.remove_handler(h);
    }
    {
        auto& h = log.add_handler(lg::file_handler(path));  // append: the first line stays
        h.set_formatter(lg::formatter("%(levelname)s:%(message)s"));
        log.error("second");
        log.remove_handler(h);
    }
    ASSERT_SAME(slurp(path), "INFO:first\nERROR:second\n");
    {
        auto& h = log.add_handler(lg::file_handler(path, false));  // truncate
        h.set_formatter(lg::formatter("%(message)s"));
        log.info("only");
        log.remove_handler(h);
    }
    ASSERT_SAME(slurp(path), "only\n");

    ASSERT_THROWS(lg::file_handler(tmp("httpp_no_such_dir_xyz") / "x" / "y.log"), std::runtime_error);

    log.set_propagate(true);
    log.set_level(lg::level::notset);
    path.unlink();
}

UTEST(httpp_logging_tests, test_callback_handler_gets_the_whole_record) {
    lg::logger& log = lg::get_logger("test.logging.record");
    log.set_level(lg::level::debug);
    std::string name, message;
    lg::level lv = lg::level::notset;
    bool same_thread = false;
    auto& h = log.add_handler(lg::callback_handler([&](const lg::record& r) {
        name = std::string(r.name);
        message = std::string(r.message);
        lv = r.levelno;
        same_thread = r.thread == std::this_thread::get_id();
    }));
    log.error("boom %d", 3);
    log.remove_handler(h);
    ASSERT_SAME(name, "test.logging.record");
    ASSERT_SAME(message, "boom 3");
    ASSERT_TRUE(lv == lg::level::error);
    ASSERT_TRUE(same_thread);
    log.set_level(lg::level::notset);
}

UTEST(httpp_logging_tests, test_many_threads_log_without_losing_or_interleaving_records) {
    lg::logger& log = lg::get_logger("test.logging.threads");
    log.set_level(lg::level::info);
    long counted = 0;  // only touched inside the handler, which runs one record at a time
    bool whole = true;
    auto& h = log.add_handler(lg::callback_handler([&](const lg::record& r) {
        ++counted;
        if (r.message.substr(0, 4) != "msg ") whole = false;
    }));
    const int threads = 4, each = 250;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&log, t, each] {  // `each` by value: MSVC does not see a constexpr as always usable
            for (int i = 0; i < each; ++i) log.info("msg %d %d", t, i);
        });
    for (std::thread& th : pool) th.join();
    log.remove_handler(h);
    ASSERT_EQ(int(counted), threads * each);
    ASSERT_TRUE(whole);
    log.set_level(lg::level::notset);
}

UTEST(httpp_logging_tests, test_basic_config_adds_one_handler_and_sets_the_root_level) {
    lg::logger& root = lg::get_logger();
    ASSERT_EQ(int(root.handler_count()), 0);
    lg::basic_config(lg::level::error);
    ASSERT_EQ(int(root.handler_count()), 1);
    ASSERT_TRUE(root.get_level() == lg::level::error);
    lg::basic_config(lg::level::warning);  // a second call adds nothing
    ASSERT_EQ(int(root.handler_count()), 1);
    root.clear_handlers();
    root.set_level(lg::level::warning);
}

// -------------------------------------------------------------------------------------- formatter

UTEST(httpp_logging_tests, test_formatter_fields_widths_and_alignment) {
    const lg::record r = make_record("abc", lg::level::warning, "hello", std::chrono::milliseconds(1234));
    ASSERT_SAME(lg::formatter("[%(levelname)-8s] %(name)s: %(message)s").format(r), "[WARNING ] abc: hello");
    ASSERT_SAME(lg::formatter("%(levelname)10s|").format(r), "   WARNING|");
    ASSERT_SAME(lg::formatter("%(levelno)d %(levelno)05d").format(r), "30 00030");
    ASSERT_SAME(lg::formatter("%(message).3s").format(r), "hel");
    ASSERT_SAME(lg::formatter("100%% %(message)s").format(r), "100% hello");
    ASSERT_SAME(lg::formatter("%(msecs)03d").format(r), "234");
    ASSERT_SAME(lg::formatter("%(created).3f").format(r), "1.234");
    ASSERT_SAME(lg::formatter("%(created)d").format(r), "1");
    ASSERT_SAME(lg::formatter("no fields at all").format(r), "no fields at all");
    ASSERT_SAME(lg::formatter("").format(r), "");
    ASSERT_TRUE(!lg::formatter("%(process)d %(thread)d").format(r).empty());
}

UTEST(httpp_logging_tests, test_formatter_asctime) {
    // 2001-09-09 01:46:40 UTC plus 234 ms: the year is 2001 and the date 8th to 10th in every time zone.
    const lg::record r = make_record("n", lg::level::info, "m", std::chrono::milliseconds(1000000000234LL));
    const std::string with_ms = lg::formatter("%(asctime)s").format(r);
    ASSERT_TRUE(with_ms.rfind("2001-09-", 0) == 0);
    ASSERT_TRUE(with_ms.size() == 23);
    ASSERT_SAME(with_ms.substr(with_ms.size() - 4), ",234");
    ASSERT_SAME(lg::formatter("%(asctime)s", "%Y").format(r), "2001");  // a datefmt: strftime only
}

UTEST(httpp_logging_tests, test_formatter_rejects_a_bad_pattern) {
    ASSERT_THROWS(lg::formatter("%(nope)s"), std::invalid_argument);
    ASSERT_THROWS(lg::formatter("%"), std::invalid_argument);
    ASSERT_THROWS(lg::formatter("abc %"), std::invalid_argument);
    ASSERT_THROWS(lg::formatter("%(name"), std::invalid_argument);
    ASSERT_THROWS(lg::formatter("%(name)"), std::invalid_argument);
    ASSERT_THROWS(lg::formatter("%(name)d"), std::invalid_argument);
    ASSERT_THROWS(lg::formatter("%(levelno)s"), std::invalid_argument);
    ASSERT_THROWS(lg::formatter("%(message)f"), std::invalid_argument);
    ASSERT_THROWS(lg::formatter("%x"), std::invalid_argument);
    ASSERT_SAME(lg::formatter("%(name)s %(levelno)d").pattern(), "%(name)s %(levelno)d");
}
