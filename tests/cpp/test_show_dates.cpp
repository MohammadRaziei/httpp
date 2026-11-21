// show_dates(false) on httpp::directory and httpp::file_browser: no file time is sent or shown anywhere (ADR-0032).
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/file_browser.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#define EXPECT_STR_EQ(a, b) EXPECT_TRUE(std::string(a) == std::string(b))

using testutil::local_server;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

struct tree {
    fs::path root;
    tree() {
        static int counter = 0;
        root = fs::temp_directory_path() / ("httpp_dates_" + std::to_string(static_cast<long>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007)) + "_" + std::to_string(counter++));
        fs::remove_all(root);
        fs::create_directories(root / "sub");
        std::ofstream(root / "a.txt") << "hello world";
        std::ofstream(root / "sub" / "b.txt") << "bee";
    }
    ~tree() { std::error_code ec; fs::remove_all(root, ec); }
};

httpp::response get(const local_server& srv, const std::string& path, const std::string& name = "", const std::string& value = "") {
    httpp::request r(srv.url(path));
    if (!name.empty()) r.header(name, value);
    return r.run();
}

// The DOS time and date of the first entry of a ZIP (the local header: signature, 6 bytes, then time at 10 and date at 12).
bool first_zip_stamp(const std::string& zip, std::uint16_t& time, std::uint16_t& date) {
    const std::size_t at = zip.find("PK\x03\x04");
    if (at == std::string::npos || zip.size() < at + 14) return false;
    auto u16 = [&](std::size_t i) { return static_cast<std::uint16_t>(static_cast<unsigned char>(zip[i]) | (static_cast<unsigned char>(zip[i + 1]) << 8)); };
    time = u16(at + 10);
    date = u16(at + 12);
    return true;
}

} // namespace

UTEST(httpp_show_dates, directory_sends_validators_and_answers_304_by_default) {
    tree t;
    local_server srv([&](httpp::server& s) { httpp::directory(t.root.string()).mount(s, "/f"); });
    auto r = get(srv, "/f/a.txt");
    ASSERT_EQ(200, r.status);
    EXPECT_FALSE(r.header("ETag").empty());
    EXPECT_FALSE(r.header("Last-Modified").empty());
    EXPECT_EQ(304, get(srv, "/f/a.txt", "If-None-Match", r.header("ETag")).status);
}

UTEST(httpp_show_dates, directory_without_dates_sends_no_validator_and_never_answers_304) {
    tree t;
    local_server srv([&](httpp::server& s) { httpp::directory(t.root.string()).show_dates(false).mount(s, "/f"); });
    auto r = get(srv, "/f/a.txt");
    ASSERT_EQ(200, r.status);
    EXPECT_STR_EQ("hello world", r.body);
    EXPECT_TRUE(r.header("ETag").empty());
    EXPECT_TRUE(r.header("Last-Modified").empty());
    EXPECT_STR_EQ("no-cache", r.header("Cache-Control"));
    EXPECT_EQ(200, get(srv, "/f/a.txt", "If-None-Match", "*").status);
    EXPECT_EQ(200, get(srv, "/f/a.txt", "If-Modified-Since", "Wed, 01 Jan 2031 00:00:00 GMT").status);
}

UTEST(httpp_show_dates, directory_without_dates_still_serves_ranges_but_cannot_check_an_if_range) {
    tree t;
    local_server srv([&](httpp::server& s) { httpp::directory(t.root.string()).show_dates(false).mount(s, "/f"); });
    httpp::request plain(srv.url("/f/a.txt"));
    auto part = plain.header("Range", "bytes=0-4").run();
    EXPECT_EQ(206, part.status);
    EXPECT_STR_EQ("hello", part.body);
    httpp::request guarded(srv.url("/f/a.txt"));
    auto whole = guarded.header("Range", "bytes=0-4").header("If-Range", "\"anything\"").run();
    EXPECT_EQ(200, whole.status);               // nothing to compare the validator with: the whole file
    EXPECT_STR_EQ("hello world", whole.body);
}

UTEST(httpp_show_dates, the_setters_chain) {
    tree t;
    httpp::directory d(t.root.string());
    EXPECT_TRUE(&d.show_dates(false) == &d);
    httpp::file_browser fb(t.root.string());
    EXPECT_TRUE(&fb.show_dates(false) == &fb);
}

UTEST(httpp_show_dates, file_browser_lists_times_and_says_so_by_default) {
    tree t;
    local_server srv([&](httpp::server& s) { httpp::file_browser(t.root.string()).zip(true).mount(s, "/files"); });
    EXPECT_TRUE(contains(get(srv, "/files/api/list").body, "\"mtime\":"));
    EXPECT_TRUE(contains(get(srv, "/files/").body, "\"dates\":true"));
    EXPECT_FALSE(get(srv, "/files/raw/a.txt").header("Last-Modified").empty());
    std::uint16_t time = 1, date = 0;
    auto zip = get(srv, "/files/api/zip?path=/&name=a.txt");
    ASSERT_EQ(200, zip.status);
    ASSERT_TRUE(first_zip_stamp(zip.body, time, date));
    EXPECT_TRUE(date != 0x0021);               // today, not 1980-01-01
}

UTEST(httpp_show_dates, file_browser_without_dates_shows_and_sends_no_time) {
    tree t;
    local_server srv([&](httpp::server& s) { httpp::file_browser(t.root.string()).zip(true).show_dates(false).mount(s, "/files"); });
    auto list = get(srv, "/files/api/list");
    ASSERT_EQ(200, list.status);
    EXPECT_TRUE(contains(list.body, "\"name\":\"a.txt\""));
    EXPECT_FALSE(contains(list.body, "mtime"));
    EXPECT_TRUE(contains(get(srv, "/files/").body, "\"dates\":false"));
    auto raw = get(srv, "/files/raw/a.txt");
    EXPECT_EQ(200, raw.status);
    EXPECT_TRUE(raw.header("ETag").empty());
    EXPECT_TRUE(raw.header("Last-Modified").empty());
    std::uint16_t time = 1, date = 0;
    auto zip = get(srv, "/files/api/zip?path=/");  // the folder, with a subfolder: every entry carries the same stamp
    ASSERT_EQ(200, zip.status);
    ASSERT_TRUE(first_zip_stamp(zip.body, time, date));
    EXPECT_EQ(0, static_cast<int>(time));
    EXPECT_EQ(0x0021, static_cast<int>(date));  // 1980-01-01, the earliest a DOS date can say
}
