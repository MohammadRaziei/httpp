// retry() on a download (ADR-0097). A try that fails in the middle of the body must not leave the file wrong: the sink puts each
// 206 where the server says it starts (Content-Range) and cuts the file back to that point, so a Range that is sent again after
// some bytes were already appended does not write them twice.
#include "utest/utest.h"
#include "httpp/download.hpp"
#include "local_server.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

const std::string kPayload = "0123456789";

std::string temp_path(const char* name) { return std::string("download_retry_") + name + ".bin"; }

void forget(const std::string& p) {
    std::remove(p.c_str());
    std::remove((p + ".part").c_str());
}

void write_file(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << data;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool exists(const std::string& path) { return std::ifstream(path).good(); }

// First `stalled` GETs send `first_bytes` of the answer and then say nothing (the client times out); every later GET is whole.
// ranges: a Range header is answered with a 206 and a Content-Range, else it is ignored. A HEAD gets the full length.
void routes(httpp::server& s, std::atomic<int>& gets, int stalled, std::size_t first_bytes, bool ranges) {
    s.get("/file", [&gets, stalled, first_bytes, ranges](const httpp::server_request& q, httpp::response& r) {
        const std::string range = q.header("Range");
        std::size_t from = 0;
        if (ranges && range.rfind("bytes=", 0) == 0) {
            from = static_cast<std::size_t>(std::stoull(range.substr(6)));
            r.status = 206;
            r.headers.emplace_back("Content-Range", "bytes " + std::to_string(from) + "-" + std::to_string(kPayload.size() - 1) + "/" + std::to_string(kPayload.size()));
        } else {
            r.status = 200;
        }
        r.headers.emplace_back("Accept-Ranges", "bytes");
        const std::string rest = kPayload.substr(from);
        if (q.method == "HEAD") {
            r.status = 200;
            r.body = kPayload;
            return;
        }
        if (gets++ < stalled) {
            const std::string head = rest.substr(0, first_bytes);
            auto sent = std::make_shared<bool>(false);
            r.stream = [head, sent](std::size_t, std::string& chunk) {
                if (!*sent) {
                    *sent = true;
                    chunk = head;
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(3000)); // the client has given up by then
                chunk.clear();
                return false;
            };
            return;
        }
        r.body = rest;
    });
    // 206 that does not say where it starts, and one that starts after the end of what we have
    s.get("/no-content-range", [](const httpp::server_request&, httpp::response& r) {
        r.status = 206;
        r.body = kPayload.substr(4);
    });
    s.get("/gap", [](const httpp::server_request&, httpp::response& r) {
        r.status = 206;
        r.headers.emplace_back("Content-Range", "bytes 4-9/10");
        r.body = kPayload.substr(4);
    });
}

} // namespace

UTEST(download_retry, the_settings_are_there_and_read_back) {
    httpp::download d(httpp::url("http://127.0.0.1:1/x"), "unused.bin");
    d.retry(2).retry_delay(3).retry_connrefused(true);
    ASSERT_EQ(2, d.retry());
    ASSERT_EQ(3, d.retry_delay());
    ASSERT_TRUE(d.retry_connrefused());
}

UTEST(download_retry, a_body_cut_off_without_resume_starts_the_file_again) {
    std::atomic<int> gets{0};
    testutil::local_server srv([&](httpp::server& s) { routes(s, gets, 1, 5, false); });
    const auto path = temp_path("plain");
    forget(path);
    auto res = httpp::download(httpp::url(srv.url("/file")), path).disable_progress().max_time(1).retry(1).retry_delay(1).run();
    ASSERT_TRUE(res.ok);
    ASSERT_STREQ(kPayload.c_str(), read_file(path).c_str());
    ASSERT_EQ(2, gets.load());
    forget(path);
}

UTEST(download_retry, a_resumed_body_cut_off_is_not_written_twice) {
    std::atomic<int> gets{0};
    testutil::local_server srv([&](httpp::server& s) { routes(s, gets, 1, 2, true); });
    const auto path = temp_path("resume");
    forget(path);
    write_file(path + ".part", "0123"); // an earlier run got this far
    auto res = httpp::download(httpp::url(srv.url("/file")), path).disable_progress().resume().max_time(1).retry(1).retry_delay(1).run();
    ASSERT_TRUE(res.ok);
    ASSERT_STREQ(kPayload.c_str(), read_file(path).c_str()); // not 0123450123456789
    ASSERT_FALSE(exists(path + ".part"));
    ASSERT_EQ(2, gets.load());
    forget(path);
}

UTEST(download_retry, a_transient_status_is_sent_again_and_its_page_is_not_written) {
    std::atomic<int> hits{0};
    testutil::local_server srv([&](httpp::server& s) {
        s.get("/flaky", [&hits](const httpp::server_request&, httpp::response& r) {
            if (hits++ == 0) {
                r.status = 503;
                r.body = "busy";
            } else {
                r.status = 200;
                r.body = kPayload;
            }
        });
    });
    const auto path = temp_path("flaky");
    forget(path);
    auto res = httpp::download(httpp::url(srv.url("/flaky")), path).disable_progress().retry(1).retry_delay(1).run();
    ASSERT_TRUE(res.ok);
    ASSERT_STREQ(kPayload.c_str(), read_file(path).c_str());
    forget(path);
}

// A server that answers 206 although no Range was sent (or without saying where the bytes start) cannot be placed in the file.

UTEST(download_retry, a_206_that_does_not_say_where_it_starts_is_an_error_not_a_guess) {
    std::atomic<int> gets{0};
    testutil::local_server srv([&](httpp::server& s) { routes(s, gets, 0, 0, true); });
    const auto path = temp_path("nocr");
    forget(path);
    auto res = httpp::download(httpp::url(srv.url("/no-content-range")), path).disable_progress().run();
    ASSERT_FALSE(res.ok);
    ASSERT_FALSE(exists(path));
    forget(path);
}

UTEST(download_retry, a_content_range_that_starts_past_the_end_of_the_file_is_an_error_not_a_hole) { // the file is empty, the answer starts at 4
    std::atomic<int> gets{0};
    testutil::local_server srv([&](httpp::server& s) { routes(s, gets, 0, 0, true); });
    const auto path = temp_path("gap");
    forget(path);
    auto res = httpp::download(httpp::url(srv.url("/gap")), path).disable_progress().run();
    ASSERT_FALSE(res.ok);
    ASSERT_FALSE(exists(path));
    forget(path);
}

// on_retry() (ADR-0109): told before each new try, as on request, so that a front end can say "will retry" before it waits.

UTEST(download_retry, on_retry_is_told_the_last_answer_the_number_and_the_wait_before_it_starts) {
    std::atomic<int> hits{0};
    testutil::local_server srv([&](httpp::server& s) {
        s.get("/flaky", [&hits](const httpp::server_request&, httpp::response& r) {
            if (hits++ == 0) {
                r.status = 503;
                r.body = "busy";
            } else {
                r.status = 200;
                r.body = kPayload;
            }
        });
    });
    const auto path = temp_path("onretry");
    forget(path);
    int calls = 0, status = 0, number = 0;
    long delay = 0;
    auto res = httpp::download(httpp::url(srv.url("/flaky")), path)
                   .disable_progress()
                   .retry(2)
                   .on_retry([&](const httpp::response& last, int n, long wait) { // no retry_delay(): 1, 2, 4, ... seconds
                       ++calls;
                       status = last.status;
                       number = n;
                       delay = wait;
                   })
                   .run();
    ASSERT_TRUE(res.ok);
    ASSERT_STREQ(kPayload.c_str(), read_file(path).c_str());
    ASSERT_EQ(1, calls);
    ASSERT_EQ(503, status);
    ASSERT_EQ(1, number);
    ASSERT_EQ(1, delay);
    forget(path);
}

UTEST(download_retry, a_download_without_on_retry_retries_silently_as_before) {
    std::atomic<int> hits{0};
    testutil::local_server srv([&](httpp::server& s) {
        s.get("/flaky", [&hits](const httpp::server_request&, httpp::response& r) {
            r.status = hits++ == 0 ? 503 : 200;
            r.body = kPayload;
        });
    });
    const auto path = temp_path("silent");
    forget(path);
    ASSERT_TRUE(httpp::download(httpp::url(srv.url("/flaky")), path).disable_progress().retry(1).retry_delay(1).run().ok);
    forget(path);
}
