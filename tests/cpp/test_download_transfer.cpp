// What a download does on the wire (ADR-0052): redirects, Range resume, servers that ignore Range, error pages, big bodies.
#include "utest/utest.h"
#include "httpp/download.hpp"
#include "httpp/sha256.hpp"
#include "local_server.hpp"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace {

const std::string& payload() {
    static const std::string p = [] {
        std::string s;
        for (int i = 0; i < 200000; ++i) s += static_cast<char>('a' + (i * 7) % 26);
        return s;
    }();
    return p;
}

struct seen {
    std::mutex mu;
    std::string range;
    int gets = 0;
};

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << data;
}

bool exists(const std::string& path) { return std::ifstream(path).good(); }

std::string temp_path(const char* name) { return std::string("download_transfer_") + name + ".bin"; }

void forget(const std::string& p) {
    std::remove(p.c_str());
    std::remove((p + ".part").c_str());
}

// honours "Range: bytes=N-" when `ranges` is true
void routes(httpp::server& s, seen& st, bool ranges) {
    s.get("/file", [&st, ranges](const httpp::server_request& q, httpp::response& r) {
        const std::string range = q.header("Range");
        {
            std::lock_guard<std::mutex> lock(st.mu);
            st.range = range;
            if (q.method != "HEAD") ++st.gets; // the server answers HEAD through this handler too
        }
        r.headers.emplace_back("Accept-Ranges", "bytes");
        if (ranges && range.rfind("bytes=", 0) == 0) {
            const std::size_t from = static_cast<std::size_t>(std::stoull(range.substr(6)));
            r.status = 206;
            r.headers.emplace_back("Content-Range", "bytes " + std::to_string(from) + "-" + std::to_string(payload().size() - 1) + "/" + std::to_string(payload().size()));
            r.body = payload().substr(from);
            return;
        }
        r.status = 200;
        r.body = payload();
    });
    s.get("/old", [](const httpp::server_request&, httpp::response& r) {
        r.status = 302;
        r.headers.emplace_back("Location", "/file");
        r.body = "moved, do not save this page";
    });
    s.get("/gone", [](const httpp::server_request&, httpp::response& r) {
        r.status = 404;
        r.body = "<html>not found</html>";
    });
}

} // namespace

UTEST(download_transfer, a_redirect_is_followed_and_only_the_final_body_is_saved) {
    seen st;
    testutil::local_server srv([&](httpp::server& s) { routes(s, st, true); });
    const auto path = temp_path("redirect");
    forget(path);
    auto res = httpp::download(srv.url("/old"), path).disable_progress().run();
    ASSERT_TRUE(res.ok);
    ASSERT_TRUE(payload() == read_file(path));
    forget(path);
}

UTEST(download_transfer, a_redirect_that_is_not_followed_fails_and_leaves_no_file) {
    seen st;
    testutil::local_server srv([&](httpp::server& s) { routes(s, st, true); });
    const auto path = temp_path("noredirect");
    forget(path);
    auto res = httpp::download(srv.url("/old"), path).disable_progress().follow_redirects(false).run();
    ASSERT_FALSE(res.ok);
    ASSERT_EQ(302, res.status);
    ASSERT_FALSE(exists(path));
}

UTEST(download_transfer, an_error_page_is_not_saved_and_resume_leaves_no_part_file) {
    seen st;
    testutil::local_server srv([&](httpp::server& s) { routes(s, st, true); });
    const auto path = temp_path("gone");
    forget(path);
    auto res = httpp::download(srv.url("/gone"), path).disable_progress().resume().run();
    ASSERT_FALSE(res.ok);
    ASSERT_EQ(404, res.status);
    ASSERT_FALSE(exists(path));
    ASSERT_FALSE(exists(path + ".part"));
}

UTEST(download_transfer, resume_continues_a_part_file_with_a_range_request) {
    seen st;
    testutil::local_server srv([&](httpp::server& s) { routes(s, st, true); });
    const auto path = temp_path("resume");
    forget(path);
    write_file(path + ".part", payload().substr(0, 70000));
    auto res = httpp::download(srv.url("/file"), path).disable_progress().resume().run();
    ASSERT_TRUE(res.ok);
    ASSERT_TRUE(payload() == read_file(path));
    ASSERT_FALSE(exists(path + ".part"));
    std::lock_guard<std::mutex> lock(st.mu);
    ASSERT_TRUE(st.range == "bytes=70000-");
    forget(path);
}

UTEST(download_transfer, a_server_that_ignores_range_makes_resume_start_over) {
    seen st;
    testutil::local_server srv([&](httpp::server& s) { routes(s, st, false); });
    const auto path = temp_path("ignored_range");
    forget(path);
    write_file(path + ".part", std::string(70000, '#')); // garbage that must not survive
    auto res = httpp::download(srv.url("/file"), path).disable_progress().resume().run();
    ASSERT_TRUE(res.ok);
    ASSERT_TRUE(payload() == read_file(path));
    forget(path);
}

UTEST(download_transfer, a_complete_file_is_skipped_without_a_get) {
    seen st;
    testutil::local_server srv([&](httpp::server& s) { routes(s, st, true); });
    const auto path = temp_path("skip");
    forget(path);
    write_file(path, payload());
    auto res = httpp::download(srv.url("/file"), path).disable_progress().resume().run();
    ASSERT_TRUE(res.ok);
    ASSERT_TRUE(res.skipped);
    std::lock_guard<std::mutex> lock(st.mu);
    ASSERT_EQ(0, st.gets);
    forget(path);
}

UTEST(download_transfer, a_large_body_is_saved_intact) {
    testutil::local_server srv([](httpp::server& s) {
        s.get("/big", [](const httpp::server_request&, httpp::response& r) {
            r.status = 200;
            r.body.assign(24u * 1024 * 1024, 'x');
        });
    });
    const auto path = temp_path("big");
    forget(path);
    auto res = httpp::download(srv.url("/big"), path).disable_progress().run();
    ASSERT_TRUE(res.ok);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    ASSERT_EQ(static_cast<std::streamoff>(24u * 1024 * 1024), static_cast<std::streamoff>(in.tellg()));
    forget(path);
}

UTEST(download_transfer, an_unwritable_destination_is_reported_and_stops_the_transfer) {
    seen st;
    testutil::local_server srv([&](httpp::server& s) { routes(s, st, true); });
    auto res = httpp::download(srv.url("/file"), "no_such_directory_for_httpp/out.bin").disable_progress().run();
    ASSERT_FALSE(res.ok);
    ASSERT_TRUE(res.error.find("could not open destination file") != std::string::npos);
}
