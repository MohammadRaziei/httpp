#include "utest/utest.h"
#include "httpp/download.hpp"
#include "httpp/sha256.hpp"
#include "httpp/server.hpp"

#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <future>

namespace {
struct running_server {
    httpp::server srv;
    std::thread th;
    int port;

    explicit running_server(httpp::server&& s) : srv(std::move(s)) {
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ~running_server() {
        srv.stop();
        if (th.joinable()) th.join();
    }
};

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

UTEST(httpp_download, downloads_body_to_a_file) {
    httpp::server s;
    s.get("/data", [](const httpp::request&, httpp::response& res) {
        res.status = 200;
        res.body = "the quick brown fox";
    });
    running_server rs(std::move(s));

    const std::string dest = "test_download_output.txt";
    std::remove(dest.c_str());

    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/data";
    httpp::download_result result = httpp::download(url, dest).disable_progress().run();

    ASSERT_TRUE(result.ok);
    ASSERT_EQ(200, result.status);
    ASSERT_STREQ("the quick brown fox", read_file(dest).c_str());

    std::remove(dest.c_str());
}

UTEST(httpp_download, reports_failure_for_404) {
    httpp::server s;
    running_server rs(std::move(s));

    const std::string dest = "test_download_missing.txt";
    std::remove(dest.c_str());

    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/nope";
    httpp::download_result result = httpp::download(url, dest).disable_progress().run();

    ASSERT_FALSE(result.ok);
    ASSERT_EQ(404, result.status);

    std::remove(dest.c_str());
}

UTEST(httpp_download, builder_form_works_with_chained_calls) {
    httpp::server s;
    s.get("/f", [](const httpp::request&, httpp::response& res) {
        res.status = 200;
        res.body = "builder";
    });
    running_server rs(std::move(s));

    const std::string dest = "test_download_builder.txt";
    std::remove(dest.c_str());

    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/f";
    httpp::download_result result = httpp::download(url)
                                         .output(dest)
                                         .disable_progress()
                                         .run();

    ASSERT_TRUE(result.ok);
    ASSERT_STREQ("builder", read_file(dest).c_str());

    std::remove(dest.c_str());
}

UTEST(httpp_download, run_async_returns_a_future) {
    httpp::server s;
    s.get("/f", [](const httpp::request&, httpp::response& res) {
        res.status = 200;
        res.body = "async";
    });
    running_server rs(std::move(s));

    const std::string dest = "test_download_async.txt";
    std::remove(dest.c_str());

    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/f";
    std::future<httpp::download_result> fut = httpp::download(url)
                                                   .output(dest)
                                                   .disable_progress()
                                                   .run_async();

    httpp::download_result result = fut.get();

    ASSERT_TRUE(result.ok);
    ASSERT_STREQ("async", read_file(dest).c_str());

    std::remove(dest.c_str());
}

// ---- sha256 / sha256_url ------------------------------------------------------------------------------------

namespace {
const std::string body_text = "the quick brown fox";
const std::string body_digest = httpp::sha256_hex(body_text.data(), body_text.size());

httpp::server file_server(const std::string& checksum_body, int checksum_status = 200) {
    httpp::server s;
    s.get("/f.bin", [](const httpp::request&, httpp::response& res) { res.status = 200; res.body = body_text; });
    s.get("/f.bin.sha256", [=](const httpp::request&, httpp::response& res) {
        res.status = checksum_status;
        res.body = checksum_body;
    });
    return s;
}
std::string url_of(const running_server& rs, const char* path = "/f.bin") {
    return "http://127.0.0.1:" + std::to_string(rs.port) + path;
}
bool exists(const std::string& path) { return std::ifstream(path).good(); }
} // namespace

UTEST(httpp_download_sha256, a_matching_digest_is_accepted_in_either_case) {
    running_server rs(file_server(""));
    const std::string dest = "test_dl_sha_ok.bin";
    std::remove(dest.c_str());
    auto res = httpp::download(url_of(rs), dest).disable_progress().sha256(body_digest).run();
    ASSERT_TRUE(res.ok);
    EXPECT_TRUE(res.sha256 == body_digest);
    EXPECT_TRUE(read_file(dest) == body_text);

    std::string upper = body_digest;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    EXPECT_TRUE(httpp::download(url_of(rs), dest).disable_progress().sha256(upper).run().ok);
    std::remove(dest.c_str());
}

UTEST(httpp_download_sha256, a_wrong_digest_fails_and_leaves_no_file) {
    running_server rs(file_server(""));
    const std::string dest = "test_dl_sha_bad.bin";
    std::remove(dest.c_str());
    auto res = httpp::download(url_of(rs), dest).disable_progress().sha256(std::string(64, '0')).run();
    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.error.find("SHA-256") != std::string::npos);
    EXPECT_TRUE(res.error.find(body_digest) != std::string::npos);  // says what it actually got
    EXPECT_FALSE(exists(dest));
}

UTEST(httpp_download_sha256, a_malformed_digest_fails_before_any_request) {
    httpp::server s;  // nothing listens for it: the check must not even get that far
    const std::string dest = "test_dl_sha_malformed.bin";
    std::remove(dest.c_str());
    auto res = httpp::download("http://127.0.0.1:1/f.bin", dest).disable_progress().sha256("abc").run();
    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.error.find("SHA-256") != std::string::npos);
    EXPECT_FALSE(exists(dest));
}

UTEST(httpp_download_sha256, the_digest_can_come_from_a_checksum_file) {
    running_server rs(file_server(body_digest + "  f.bin\n"));
    const std::string dest = "test_dl_sha_url.bin";
    std::remove(dest.c_str());
    auto res = httpp::download(url_of(rs), dest).disable_progress().sha256_url().run();  // default: <url>.sha256
    ASSERT_TRUE(res.ok);
    EXPECT_TRUE(res.sha256 == body_digest);
    EXPECT_FALSE(exists(dest + ".sha256.part"));  // the temporary checksum file is gone
    std::remove(dest.c_str());

    auto explicit_url = httpp::download(url_of(rs), dest).disable_progress().sha256_url(url_of(rs, "/f.bin.sha256")).run();
    EXPECT_TRUE(explicit_url.ok);
    std::remove(dest.c_str());
}

UTEST(httpp_download_sha256, a_bad_checksum_file_is_an_error_and_nothing_is_downloaded) {
    const std::string dest = "test_dl_sha_url_bad.bin";
    {   // an HTML page where the digest should be
        running_server rs(file_server("<!DOCTYPE html><html>Not Found</html>"));
        std::remove(dest.c_str());
        auto res = httpp::download(url_of(rs), dest).disable_progress().sha256_url().run();
        EXPECT_FALSE(res.ok);
        EXPECT_FALSE(exists(dest));
    }
    {   // the file names another file
        running_server rs(file_server(body_digest + "  other.bin\n"));
        EXPECT_FALSE(httpp::download(url_of(rs), dest).disable_progress().sha256_url().run().ok);
        EXPECT_FALSE(exists(dest));
    }
    {   // no checksum file at all
        running_server rs(file_server("", 404));
        auto res = httpp::download(url_of(rs), dest).disable_progress().sha256_url().run();
        EXPECT_FALSE(res.ok);
        EXPECT_TRUE(res.error.find("checksum") != std::string::npos);
        EXPECT_FALSE(exists(dest));
    }
    {   // a wrong digest in a well-formed checksum file
        running_server rs(file_server(std::string(64, '1') + "\n"));
        EXPECT_FALSE(httpp::download(url_of(rs), dest).disable_progress().sha256_url().run().ok);
        EXPECT_FALSE(exists(dest));
    }
}

UTEST(httpp_download_sha256, resume_does_not_trust_a_complete_looking_but_corrupt_file) {
    running_server rs(file_server(""));
    const std::string dest = "test_dl_sha_corrupt.bin";
    { std::ofstream(dest, std::ios::binary) << std::string(body_text.size(), 'X'); }  // right size, wrong bytes
    auto res = httpp::download(url_of(rs), dest).disable_progress().resume().sha256(body_digest).run();
    ASSERT_TRUE(res.ok);
    EXPECT_FALSE(res.skipped);              // it was downloaded again
    EXPECT_TRUE(read_file(dest) == body_text);

    auto again = httpp::download(url_of(rs), dest).disable_progress().resume().sha256(body_digest).run();
    EXPECT_TRUE(again.ok);
    EXPECT_TRUE(again.skipped);             // a verified file is left alone
    std::remove(dest.c_str());
}

// ---- lock() --------------------------------------------------------------------------------------------------
// The lock is the directory <dest>.lock with a file `alive` in it that the owner rewrites; these tests play both roles.

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <atomic>

namespace {
void make_lock_dir(const std::string& dir) {
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0777);
#endif
}
void remove_lock_dir(const std::string& dir) {
    std::remove((dir + "/alive").c_str());
#ifdef _WIN32
    _rmdir(dir.c_str());
#else
    rmdir(dir.c_str());
#endif
}

// Someone else's live lock: holds <dest>.lock and rewrites `alive` until release() (or the destructor).
struct fake_owner {
    std::string dir;
    std::atomic<bool> beating{true};
    std::thread th;
    explicit fake_owner(const std::string& dest) : dir(dest + ".lock") {
        make_lock_dir(dir);
        th = std::thread([this] {
            for (int n = 1; beating; ++n) {
                { std::ofstream(dir + "/alive", std::ios::trunc) << "99999 " << n << "\n"; }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    void release() {
        beating = false;
        if (th.joinable()) th.join();
        remove_lock_dir(dir);
    }
    ~fake_owner() { release(); }
};

httpp::download_lock_options quick() {
    httpp::download_lock_options o;
    o.heartbeat = std::chrono::milliseconds(20);
    o.stale_after = std::chrono::milliseconds(300);
    o.poll = std::chrono::milliseconds(10);
    o.max_wait = std::chrono::seconds(30);
    return o;
}
double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}
} // namespace

UTEST(httpp_download_lock, a_free_lock_is_taken_and_removed_again) {
    running_server rs(file_server(""));
    const std::string dest = "test_dl_lock_free.bin";
    std::remove(dest.c_str());
    auto res = httpp::download(url_of(rs), dest).disable_progress().lock(quick()).run();
    ASSERT_TRUE(res.ok);
    EXPECT_TRUE(read_file(dest) == body_text);
    EXPECT_FALSE(exists(dest + ".lock/alive"));
    make_lock_dir(dest + ".lock");  // it can be created again, so the directory is gone
    remove_lock_dir(dest + ".lock");
    std::remove(dest.c_str());
}

UTEST(httpp_download_lock, waits_for_a_live_lock_and_then_downloads) {
    running_server rs(file_server(""));
    const std::string dest = "test_dl_lock_wait.bin";
    std::remove(dest.c_str());
    fake_owner owner(dest);
    std::thread releaser([&] { std::this_thread::sleep_for(std::chrono::milliseconds(500)); owner.release(); });
    const auto t0 = std::chrono::steady_clock::now();
    auto res = httpp::download(url_of(rs), dest).disable_progress().lock(quick()).run();  // stale_after is 300 ms:
    releaser.join();                                                                       // a live lock is NOT taken over
    ASSERT_TRUE(res.ok);
    EXPECT_TRUE(seconds_since(t0) >= 0.4);
    EXPECT_TRUE(read_file(dest) == body_text);
    std::remove(dest.c_str());
}

UTEST(httpp_download_lock, a_lock_without_a_heartbeat_is_taken_over) {
    running_server rs(file_server(""));
    const std::string dest = "test_dl_lock_stale.bin";
    std::remove(dest.c_str());
    make_lock_dir(dest + ".lock");  // its owner died: nobody rewrites `alive`
    { std::ofstream(dest + ".lock/alive") << "12345 7\n"; }
    const auto t0 = std::chrono::steady_clock::now();
    auto res = httpp::download(url_of(rs), dest).disable_progress().lock(quick()).run();
    ASSERT_TRUE(res.ok);
    EXPECT_TRUE(seconds_since(t0) < 10.0);
    EXPECT_TRUE(read_file(dest) == body_text);
    std::remove(dest.c_str());
    make_lock_dir(dest + ".lock");  // and the taken-over lock was released at the end
    remove_lock_dir(dest + ".lock");
}

UTEST(httpp_download_lock, gives_up_waiting_for_a_live_lock_after_max_wait) {
    running_server rs(file_server(""));
    const std::string dest = "test_dl_lock_maxwait.bin";
    std::remove(dest.c_str());
    fake_owner owner(dest);  // alive for the whole test
    auto o = quick();
    o.max_wait = std::chrono::milliseconds(300);
    const auto t0 = std::chrono::steady_clock::now();
    auto res = httpp::download(url_of(rs), dest).disable_progress().lock(o).run();
    ASSERT_TRUE(res.ok);
    EXPECT_TRUE(seconds_since(t0) >= 0.25);
    EXPECT_TRUE(exists(dest + ".lock/alive"));  // it was not ours, so it is not removed
    std::remove(dest.c_str());
}

UTEST(httpp_download_lock, a_waiter_uses_the_file_the_owner_finished_without_a_request) {
    std::atomic<int> requests{0};
    httpp::server s;
    s.get("/f.bin", [&](const httpp::request&, httpp::response& res) { ++requests; res.status = 200; res.body = body_text; });
    running_server rs(std::move(s));
    const std::string dest = "test_dl_lock_shared.bin";
    std::remove(dest.c_str());
    fake_owner owner(dest);
    std::thread owner_finishes([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        { std::ofstream(dest, std::ios::binary) << body_text; }  // the owner's download lands in the cache
        owner.release();
    });
    auto res = httpp::download(url_of(rs), dest).disable_progress().lock(quick()).sha256(body_digest).run();
    owner_finishes.join();
    ASSERT_TRUE(res.ok);
    EXPECT_TRUE(res.skipped);
    EXPECT_EQ(0, requests.load());
    std::remove(dest.c_str());
}

UTEST(httpp_download_lock, two_downloads_of_one_file_fetch_it_once) {
    httpp::server s;
    s.get("/f.bin", [](const httpp::request&, httpp::response& res) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // long enough for the two to overlap
        res.status = 200;
        res.body = body_text;
    });
    running_server rs(std::move(s));
    const std::string dest = "test_dl_lock_two.bin";
    std::remove(dest.c_str());
    auto make = [&] { return httpp::download(url_of(rs), dest).disable_progress().lock(quick()).resume().sha256(body_digest); };
    auto a = make().run_async();
    auto b = make().run_async();
    const auto ra = a.get();
    const auto rb = b.get();
    ASSERT_TRUE(ra.ok);
    ASSERT_TRUE(rb.ok);
    EXPECT_EQ(1, int(ra.skipped) + int(rb.skipped));  // exactly one of them did the download
    EXPECT_TRUE(read_file(dest) == body_text);
    std::remove(dest.c_str());
}
