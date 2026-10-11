// Library logging (httpp/log.hpp, built on AixLog): levels, callback, file output, no clog hijack.
#include "utest/utest.h"
#include "httpp/log.hpp"
#include "local_server.hpp"
#include "httpp/client.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

struct captured {
    std::mutex mu;
    std::vector<std::pair<httpp::log_level, std::string>> lines;
    std::string where;
    bool contains(const std::string& needle) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& l : lines) if (l.second.find(needle) != std::string::npos) return true;
        return false;
    }
};

// Restores the defaults when a test ends, so tests do not leak logging state into each other.
struct log_guard {
    ~log_guard() {
        httpp::set_log_callback({});
        httpp::set_log_level(httpp::log_level::warning);
    }
};

void install(captured& c) {
    httpp::set_log_callback([&c](httpp::log_level l, const std::string& where, const std::string& msg) {
        std::lock_guard<std::mutex> lk(c.mu);
        c.lines.emplace_back(l, msg);
        c.where = where;
    });
}

// A route that throws, with no exception handler: the library logs it and answers 500.
void trigger_warning() {
    testutil::local_server srv([](httpp::server& s) {
        s.get("/boom", [](const httpp::request&, httpp::response&) { throw std::runtime_error("kaboom"); });
    });
    auto r = httpp::client::fetch(srv.url("/boom"));
    (void)r;
}

} // namespace

UTEST(log, a_swallowed_handler_exception_is_logged_as_a_warning) {
    log_guard g;
    captured c;
    install(c);
    httpp::set_log_level(httpp::log_level::warning);
    trigger_warning();
    ASSERT_TRUE(c.contains("kaboom"));
    ASSERT_TRUE(c.contains("/boom"));
    std::lock_guard<std::mutex> lk(c.mu);
    ASSERT_TRUE(c.lines.front().first == httpp::log_level::warning);
    ASSERT_FALSE(c.where.empty());
}

UTEST(log, the_level_filters_messages) {
    log_guard g;
    captured c;
    install(c);
    httpp::set_log_level(httpp::log_level::error); // a warning is below this
    trigger_warning();
    ASSERT_FALSE(c.contains("kaboom"));
    httpp::set_log_level(httpp::log_level::off);
    trigger_warning();
    ASSERT_FALSE(c.contains("kaboom"));
    httpp::set_log_level(httpp::log_level::warning);
    trigger_warning();
    ASSERT_TRUE(c.contains("kaboom"));
}

UTEST(log, debug_level_shows_connection_level_detail) {
    log_guard g;
    captured c;
    install(c);
    httpp::set_log_level(httpp::log_level::trace);
    trigger_warning();
    std::lock_guard<std::mutex> lk(c.mu);
    ASSERT_TRUE(c.lines.size() >= 2); // the warning plus trace/debug lines (connection closed ...)
}

UTEST(log, can_write_to_a_file_and_back_to_stderr) {
    log_guard g;
    const std::string path = "httpp_test_log.txt";
    std::remove(path.c_str());
    ASSERT_TRUE(httpp::set_log_file(path));
    httpp::set_log_level(httpp::log_level::warning);
    trigger_warning();
    httpp::set_log_file(""); // closes and flushes the file sink
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    ASSERT_TRUE(text.str().find("kaboom") != std::string::npos);
    ASSERT_TRUE(text.str().find("httpp") != std::string::npos); // the tag
    std::remove(path.c_str());
}

UTEST(log, an_unwritable_file_is_reported_and_keeps_the_previous_output) {
    log_guard g;
    captured c;
    install(c);
    ASSERT_FALSE(httpp::set_log_file("/no/such/dir/x.log"));
    trigger_warning();
    ASSERT_TRUE(c.contains("kaboom")); // still going to the callback
}

UTEST(log, std_clog_is_left_alone) {
    // Whatever the library does, it must neither redirect the process-wide std::clog nor write into it.
    std::ostringstream mine;
    std::streambuf* original = std::clog.rdbuf(mine.rdbuf());
    {
        log_guard g;
        captured c;
        install(c);
        trigger_warning();
        httpp::set_log_callback({}); // back to the default sink (stderr)
        trigger_warning();
    }
    std::streambuf* during = std::clog.rdbuf(original);
    ASSERT_TRUE(during == mine.rdbuf()); // still our buffer: nobody swapped it
    ASSERT_TRUE(mine.str().empty());     // and nothing was written through it
}
