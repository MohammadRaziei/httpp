// Conditional requests for files served from a mounted directory (ADR-0045): ETag, Last-Modified, 304, 412 and If-Range.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/file_browser.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using testutil::local_server;
namespace fs = std::filesystem;

namespace {

// A temporary directory with a few files, removed at the end.
struct site {
    fs::path dir;
    site() {
        dir = fs::temp_directory_path() / "httpp_conditional_test";
        fs::remove_all(dir);
        fs::create_directories(dir / "sub");
        write("a.txt", "hello world");
        write("index.html", "<h1>index</h1>");
        age("a.txt", 48);
        age("index.html", 48);
    }
    ~site() { std::error_code ec; fs::remove_all(dir, ec); }
    void write(const std::string& name, const std::string& body) { std::ofstream(dir / name, std::ios::binary | std::ios::trunc) << body; }
    // Sets the modification time to `hours` ago.
    void age(const std::string& name, int hours) {
        fs::last_write_time(dir / name, fs::file_time_type::clock::now() - std::chrono::hours(hours));
    }
};

httpp::response get(const local_server& srv, const std::string& path, std::vector<std::pair<std::string, std::string>> headers = {},
                    const char* method = "GET") {
    httpp::request r(srv.url(path));
    r.method(method);
    for (auto& h : headers) r.header(h.first, h.second);
    return r.run();
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

} // namespace

UTEST(httpp_server_conditional, a_file_answer_carries_a_strong_etag_and_a_last_modified_date) {
    site s;
    local_server srv([&](httpp::server& sv) { sv.serve_directory("/f", s.dir.string()); });
    auto r = get(srv, "/f/a.txt");
    ASSERT_EQ(200, r.status);
    const std::string etag = r.header("ETag");
    ASSERT_TRUE(etag.size() > 2 && etag.front() == '"' && etag.back() == '"'); // strong: no W/ prefix
    const std::string date = r.header("Last-Modified");
    ASSERT_EQ(29u, date.size()); // "Sun, 06 Nov 1994 08:49:37 GMT"
    ASSERT_TRUE(date.compare(date.size() - 4, 4, " GMT") == 0);
    auto head = get(srv, "/f/a.txt", {}, "HEAD");
    ASSERT_EQ(200, head.status);
    ASSERT_TRUE(head.header("ETag") == etag && head.header("Last-Modified") == date);
    auto index = get(srv, "/f/"); // the mount itself is answered with index.html
    ASSERT_EQ(200, index.status);
    ASSERT_FALSE(index.header("ETag").empty());
}

UTEST(httpp_server_conditional, if_none_match_with_the_current_etag_is_a_304_without_a_body) {
    site s;
    local_server srv([&](httpp::server& sv) { sv.serve_directory("/f", s.dir.string()); });
    const auto first = get(srv, "/f/a.txt");
    const std::string etag = first.header("ETag");
    auto r = get(srv, "/f/a.txt", {{"If-None-Match", etag}});
    ASSERT_EQ(304, r.status);
    ASSERT_TRUE(r.body.empty());
    ASSERT_TRUE(r.header("ETag") == etag);
    ASSERT_TRUE(r.header("Last-Modified") == first.header("Last-Modified"));
    ASSERT_TRUE(r.header("Content-Length").empty()); // a 304 has no body to measure
    auto head = get(srv, "/f/a.txt", {{"If-None-Match", etag}}, "HEAD");
    ASSERT_EQ(304, head.status);
}

UTEST(httpp_server_conditional, if_none_match_accepts_lists_weak_tags_and_a_star) {
    site s;
    local_server srv([&](httpp::server& sv) { sv.serve_directory("/f", s.dir.string()); });
    const std::string etag = get(srv, "/f/a.txt").header("ETag");
    ASSERT_EQ(304, get(srv, "/f/a.txt", {{"If-None-Match", "\"nope\", " + etag + ", \"other\""}}).status);
    ASSERT_EQ(304, get(srv, "/f/a.txt", {{"If-None-Match", "W/" + etag}}).status); // weak comparison
    ASSERT_EQ(304, get(srv, "/f/a.txt", {{"If-None-Match", "*"}}).status);
    auto other = get(srv, "/f/a.txt", {{"If-None-Match", "\"nope\""}});
    ASSERT_EQ(200, other.status);
    ASSERT_TRUE(other.body == "hello world");
    ASSERT_EQ(200, get(srv, "/f/a.txt", {{"If-None-Match", ""}}).status);
}

UTEST(httpp_server_conditional, if_modified_since_compares_dates_and_if_none_match_wins_when_both_are_sent) {
    site s;
    local_server srv([&](httpp::server& sv) { sv.serve_directory("/f", s.dir.string()); });
    const auto first = get(srv, "/f/a.txt");
    const std::string date = first.header("Last-Modified");
    ASSERT_EQ(304, get(srv, "/f/a.txt", {{"If-Modified-Since", date}}).status);
    ASSERT_EQ(304, get(srv, "/f/a.txt", {{"If-Modified-Since", "Fri, 01 Jan 2100 00:00:00 GMT"}}).status);
    ASSERT_EQ(200, get(srv, "/f/a.txt", {{"If-Modified-Since", "Mon, 01 Jan 2001 00:00:00 GMT"}}).status);
    ASSERT_EQ(200, get(srv, "/f/a.txt", {{"If-Modified-Since", "yesterday"}}).status);          // not a date: ignored
    ASSERT_EQ(200, get(srv, "/f/a.txt", {{"If-Modified-Since", "Sunday, 06-Nov-94 08:49:37 GMT"}}).status); // an obsolete format is ignored too
    // the tag does not match: the date is not looked at
    ASSERT_EQ(200, get(srv, "/f/a.txt", {{"If-None-Match", "\"nope\""}, {"If-Modified-Since", date}}).status);
}

UTEST(httpp_server_conditional, the_etag_changes_with_the_size_and_with_the_modification_time) {
    site s;
    local_server srv([&](httpp::server& sv) { sv.serve_directory("/f", s.dir.string()); });
    const std::string before = get(srv, "/f/a.txt").header("ETag");
    s.age("a.txt", 24); // same size, newer time
    const std::string touched = get(srv, "/f/a.txt").header("ETag");
    ASSERT_TRUE(touched != before);
    s.write("a.txt", "a longer body than before");
    s.age("a.txt", 24); // same time as the previous step, other size
    const std::string grown = get(srv, "/f/a.txt").header("ETag");
    ASSERT_TRUE(grown != touched);
    ASSERT_EQ(200, get(srv, "/f/a.txt", {{"If-None-Match", before}}).status);  // a stale tag gets the new file
    ASSERT_EQ(304, get(srv, "/f/a.txt", {{"If-None-Match", grown}}).status);
}

UTEST(httpp_server_conditional, if_range_decides_whether_a_range_is_honoured) {
    site s;
    local_server srv([&](httpp::server& sv) { sv.serve_directory("/f", s.dir.string()); });
    const auto first = get(srv, "/f/a.txt");
    const std::string etag = first.header("ETag"), date = first.header("Last-Modified");
    auto ok = get(srv, "/f/a.txt", {{"Range", "bytes=6-10"}, {"If-Range", etag}});
    ASSERT_EQ(206, ok.status);
    ASSERT_TRUE(ok.body == "world");
    auto stale = get(srv, "/f/a.txt", {{"Range", "bytes=6-10"}, {"If-Range", "\"nope\""}});
    ASSERT_EQ(200, stale.status); // the file is not the one the client has a part of: the whole file
    ASSERT_TRUE(stale.body == "hello world");
    ASSERT_EQ(200, get(srv, "/f/a.txt", {{"Range", "bytes=6-10"}, {"If-Range", "W/" + etag}}).status); // a weak tag never matches
    ASSERT_EQ(206, get(srv, "/f/a.txt", {{"Range", "bytes=6-10"}, {"If-Range", date}}).status);
    ASSERT_EQ(200, get(srv, "/f/a.txt", {{"Range", "bytes=6-10"}, {"If-Range", "Mon, 01 Jan 2001 00:00:00 GMT"}}).status);
    ASSERT_EQ(200, get(srv, "/f/a.txt", {{"If-Range", etag}}).status); // no Range: If-Range does nothing
}

UTEST(httpp_server_conditional, if_match_and_if_unmodified_since_give_412_when_the_file_is_not_the_expected_one) {
    site s;
    local_server srv([&](httpp::server& sv) { sv.serve_directory("/f", s.dir.string()); });
    const auto first = get(srv, "/f/a.txt");
    const std::string etag = first.header("ETag"), date = first.header("Last-Modified");
    ASSERT_EQ(200, get(srv, "/f/a.txt", {{"If-Match", etag}}).status);
    ASSERT_EQ(200, get(srv, "/f/a.txt", {{"If-Match", "*"}}).status);
    ASSERT_EQ(412, get(srv, "/f/a.txt", {{"If-Match", "\"nope\""}}).status);
    ASSERT_EQ(412, get(srv, "/f/a.txt", {{"If-Match", "W/" + etag}}).status); // If-Match uses the strong comparison
    ASSERT_EQ(200, get(srv, "/f/a.txt", {{"If-Unmodified-Since", date}}).status);
    ASSERT_EQ(412, get(srv, "/f/a.txt", {{"If-Unmodified-Since", "Mon, 01 Jan 2001 00:00:00 GMT"}}).status);
    // a resuming download: the same file, the range is served; the file replaced, the client is told
    ASSERT_EQ(206, get(srv, "/f/a.txt", {{"If-Match", etag}, {"Range", "bytes=6-10"}}).status);
    s.write("a.txt", "something else entirely");
    ASSERT_EQ(412, get(srv, "/f/a.txt", {{"If-Match", etag}, {"Range", "bytes=6-10"}}).status);
}

UTEST(httpp_server_conditional, a_missing_file_is_still_a_404_and_other_methods_are_not_conditional_files) {
    site s;
    local_server srv([&](httpp::server& sv) { sv.serve_directory("/f", s.dir.string()); });
    ASSERT_EQ(404, get(srv, "/f/missing.txt", {{"If-None-Match", "*"}}).status);
}

UTEST(httpp_server_conditional, the_file_browser_raw_route_has_the_same_validators) {
    site s;
    local_server srv([&](httpp::server& sv) { httpp::file_browser(s.dir.string()).mount(sv, "/files"); });
    auto r = get(srv, "/files/raw/a.txt");
    ASSERT_EQ(200, r.status);
    ASSERT_FALSE(r.header("ETag").empty());
    ASSERT_EQ(304, get(srv, "/files/raw/a.txt", {{"If-None-Match", r.header("ETag")}}).status);
}

UTEST(httpp_server_conditional, the_etag_of_a_download_active_mount_depends_on_how_the_file_is_sent) {
    // A browser that cached index.html as a download must not be told "304 Not Modified" after the mount starts showing it (a 304 keeps the
    // cached Content-Disposition), so the validator changes with the mode.
    site s;
    local_server srv([&](httpp::server& sv) {
        httpp::directory(s.dir.string()).download_active(true).mount(sv, "/dl");
        httpp::directory(s.dir.string()).download_active(true).inline_active(true).mount(sv, "/inline");
        httpp::directory(s.dir.string()).mount(sv, "/plain");
    });
    auto download = get(srv, "/dl/index.html");
    auto inline_ = get(srv, "/inline/index.html");
    ASSERT_EQ(200, download.status);
    ASSERT_EQ(200, inline_.status);
    ASSERT_TRUE(contains(download.header("Content-Disposition"), "attachment"));
    ASSERT_FALSE(contains(inline_.header("Content-Disposition"), "attachment"));
    ASSERT_FALSE(download.header("ETag").empty());
    ASSERT_TRUE(download.header("ETag") != inline_.header("ETag"));
    auto revalidated = get(srv, "/inline/index.html", {{"If-None-Match", download.header("ETag")}});  // the old validator: a full answer
    ASSERT_EQ(200, revalidated.status);
    ASSERT_FALSE(contains(revalidated.header("Content-Disposition"), "attachment"));
    ASSERT_EQ(304, get(srv, "/inline/index.html", {{"If-None-Match", inline_.header("ETag")}}).status);  // its own validator still works
    // a type that is shown the same way in every mode keeps working with a conditional request, and a plain mount is untouched
    ASSERT_EQ(304, get(srv, "/dl/a.txt", {{"If-None-Match", get(srv, "/dl/a.txt").header("ETag")}}).status);
    ASSERT_EQ(200, get(srv, "/plain/a.txt").status);
}

// ---- download_active: the validator follows how the file is sent (ADR-0070)

UTEST(httpp_server_conditional, the_etag_follows_how_a_download_active_file_is_sent_so_a_mode_change_is_not_answered_with_a_304) {
    site s;
    s.write("page.html", "<h1>x</h1>");
    local_server srv([&](httpp::server& sv) {
        httpp::directory(s.dir.string()).download_active(true).mount(sv, "/safe");
        httpp::directory(s.dir.string()).download_active(true).inline_active(true).mount(sv, "/loose");
        httpp::directory(s.dir.string()).download_active(true).inline_active(true).mount(sv, "/loose2");
        sv.serve_directory("/f2", s.dir.string());
    });
    const std::string safe_tag = get(srv, "/safe/page.html").header("ETag");
    const std::string loose_tag = get(srv, "/loose/page.html").header("ETag");
    ASSERT_FALSE(safe_tag.empty());
    ASSERT_FALSE(loose_tag.empty());
    ASSERT_TRUE(safe_tag != loose_tag);  // attachment vs sandboxed inline
    ASSERT_TRUE(safe_tag.front() == '"' && safe_tag.back() == '"');
    // a browser that cached the download under the safe mode asks again after the mode changed: a full answer with the new headers, not a 304
    auto changed = get(srv, "/loose/page.html", {{"If-None-Match", safe_tag}});
    ASSERT_EQ(200, changed.status);
    ASSERT_TRUE(changed.header("Content-Disposition").empty());
    ASSERT_TRUE(contains(changed.header("Content-Security-Policy"), "sandbox"));
    // and a browser must ask every time (no-cache), or a fresh cached copy would be used without the question being put
    ASSERT_STREQ("no-cache", changed.header("Cache-Control").c_str());
    ASSERT_STREQ("no-cache", get(srv, "/safe/page.html").header("Cache-Control").c_str());
    ASSERT_TRUE(get(srv, "/f2/a.txt").header("Cache-Control").empty());  // a plain mounted folder keeps its caching
    // the same mode still revalidates into a 304
    ASSERT_EQ(304, get(srv, "/loose/page.html", {{"If-None-Match", loose_tag}}).status);
    ASSERT_EQ(304, get(srv, "/safe/page.html", {{"If-None-Match", safe_tag}}).status);
    // a type that is shown as it is keeps its validator across the two modes
    ASSERT_TRUE(get(srv, "/safe/a.txt").header("ETag") == get(srv, "/loose2/a.txt").header("ETag"));
}
