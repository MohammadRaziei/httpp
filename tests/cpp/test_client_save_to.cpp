#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

// request::save_to(), save_in(), create_dirs(), overwrite() and include_head(): the body of the final response goes to a file
// as it arrives (ADR-0102). The file is opened only when the final head is accepted, so a redirect, an answer that is sent
// again (retry) or an on_head() that says no leaves nothing behind. Unlike httpp::download, an error page is an answer: a 404
// body is written, as curl -o does.

namespace {
namespace fs = std::filesystem;

struct scratch_dir {
    fs::path dir;
    scratch_dir() {
        static int counter = 0;
        dir = fs::temp_directory_path() /
              ("httpp_save_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000007) + "_" + std::to_string(counter++));
        fs::create_directories(dir);
    }
    ~scratch_dir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string at(const std::string& name) const { return (dir / name).string(); }
};

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void write_file(const std::string& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

void serve(httpp::server& s) {
    s.get("/hello", [](const httpp::server_request&, httpp::response& res) { res.status = 200; res.body = "hello file"; });
    s.get("/missing", [](const httpp::server_request&, httpp::response& res) { res.status = 404; res.body = "not here"; });
    s.get("/old", [](const httpp::server_request&, httpp::response& res) {
        res.status = 302;
        res.headers.emplace_back("Location", "/hello");
        res.body = "moved";
    });
    s.get("/named", [](const httpp::server_request&, httpp::response& res) {
        res.status = 200;
        res.headers.emplace_back("Content-Disposition", "attachment; filename=\"named.txt\"");
        res.body = "named body";
    });
}
} // namespace

UTEST(httpp_client_save_to, the_body_goes_to_the_file_and_not_into_the_response) {
    testutil::local_server ls(serve);
    scratch_dir tmp;
    httpp::response res = httpp::request(ls.url("/hello")).save_to(tmp.at("a.txt")).run();
    ASSERT_TRUE(res.ok());
    ASSERT_EQ(200, res.status);
    ASSERT_TRUE(res.body.empty());
    ASSERT_STREQ("hello file", slurp(tmp.at("a.txt")).c_str());
    ASSERT_EQ(static_cast<std::size_t>(10), res.bytes_received);
    ASSERT_STREQ(tmp.at("a.txt").c_str(), res.saved_as.c_str());
}

UTEST(httpp_client_save_to, an_error_page_is_an_answer_and_is_written_like_curl_does) {
    testutil::local_server ls(serve);
    scratch_dir tmp;
    httpp::response res = httpp::request(ls.url("/missing")).save_to(tmp.at("e.txt")).run();
    ASSERT_EQ(404, res.status);
    ASSERT_STREQ("not here", slurp(tmp.at("e.txt")).c_str());
}

UTEST(httpp_client_save_to, a_redirect_that_is_followed_leaves_only_the_final_body) {
    testutil::local_server ls(serve);
    scratch_dir tmp;
    httpp::response res = httpp::request(ls.url("/old")).max_redirects(3).save_to(tmp.at("r.txt")).run();
    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("hello file", slurp(tmp.at("r.txt")).c_str());
}

UTEST(httpp_client_save_to, an_on_head_that_says_no_creates_no_file) {
    testutil::local_server ls(serve);
    scratch_dir tmp;
    httpp::response res = httpp::request(ls.url("/missing"))
                              .save_to(tmp.at("n.txt"))
                              .on_head([](const httpp::response& head) { return head.status < 400; }) // curl -f
                              .run();
    ASSERT_EQ(404, res.status);
    ASSERT_FALSE(fs::exists(tmp.at("n.txt")));
    ASSERT_TRUE(res.saved_as.empty());
}

UTEST(httpp_client_save_to, a_request_that_never_connects_creates_no_file) {
    std::string dead;
    {
        testutil::local_server gone([](httpp::server&) {});
        dead = gone.url("/x");
    }
    scratch_dir tmp;
    httpp::response res = httpp::request(dead).save_to(tmp.at("x.txt")).run();
    ASSERT_TRUE(res.error == httpp::error_kind::connection);
    ASSERT_FALSE(fs::exists(tmp.at("x.txt")));
}

UTEST(httpp_client_save_to, an_existing_file_is_replaced_by_default) {
    testutil::local_server ls(serve);
    scratch_dir tmp;
    write_file(tmp.at("o.txt"), "old and much longer content");
    ASSERT_TRUE(httpp::request(ls.url("/hello")).save_to(tmp.at("o.txt")).run().ok());
    ASSERT_STREQ("hello file", slurp(tmp.at("o.txt")).c_str());
}

UTEST(httpp_client_save_to, overwrite_false_keeps_the_file_and_fails_with_a_file_error) {
    testutil::local_server ls(serve);
    scratch_dir tmp;
    write_file(tmp.at("keep.txt"), "mine");
    httpp::response res = httpp::request(ls.url("/hello")).save_to(tmp.at("keep.txt")).overwrite(false).run();
    ASSERT_TRUE(res.error == httpp::error_kind::file);
    ASSERT_STREQ("file", res.error_name());
    ASSERT_NE(std::string::npos, res.error_message.find("Refusing to overwrite"));
    ASSERT_NE(std::string::npos, res.error_message.find("File exists"));
    ASSERT_STREQ("mine", slurp(tmp.at("keep.txt")).c_str());
}

UTEST(httpp_client_save_to, a_missing_folder_is_an_error_unless_create_dirs_is_on) {
    testutil::local_server ls(serve);
    scratch_dir tmp;
    httpp::response without = httpp::request(ls.url("/hello")).save_to(tmp.at("a/b/c.txt")).run();
    ASSERT_TRUE(without.error == httpp::error_kind::file);
    ASSERT_NE(std::string::npos, without.error_message.find("cannot write"));

    httpp::response with = httpp::request(ls.url("/hello")).save_to(tmp.at("a/b/c.txt")).create_dirs(true).run();
    ASSERT_TRUE(with.ok());
    ASSERT_STREQ("hello file", slurp(tmp.at("a/b/c.txt")).c_str());
}

UTEST(httpp_client_save_to, a_path_that_cannot_be_opened_is_a_file_error_with_the_path_in_the_message) {
    testutil::local_server ls(serve);
    scratch_dir tmp; // a folder is no file
    httpp::response res = httpp::request(ls.url("/hello")).save_to(tmp.dir.string()).run();
    ASSERT_TRUE(res.error == httpp::error_kind::file);
    ASSERT_NE(std::string::npos, res.error_message.find(tmp.dir.string()));
}

UTEST(httpp_client_save_to, include_head_puts_the_status_and_headers_in_front_of_the_body) {
    testutil::local_server ls(serve);
    scratch_dir tmp;
    httpp::response res = httpp::request(ls.url("/hello")).save_to(tmp.at("h.txt")).include_head(true).run();
    ASSERT_TRUE(res.ok());
    const std::string text = slurp(tmp.at("h.txt"));
    ASSERT_EQ(static_cast<std::size_t>(0), text.find("HTTP/1.1 200\r\n"));
    const auto blank = text.find("\r\n\r\n");
    ASSERT_NE(std::string::npos, blank);
    ASSERT_STREQ("hello file", text.substr(blank + 4).c_str());
    ASSERT_EQ(static_cast<std::size_t>(10), res.bytes_received); // the head is not body
}

UTEST(httpp_client_save_to, on_body_still_sees_every_piece_that_goes_to_the_file) {
    testutil::local_server ls(serve);
    scratch_dir tmp;
    std::string seen;
    ASSERT_TRUE(httpp::request(ls.url("/hello")).save_to(tmp.at("t.txt")).on_body([&](std::string_view piece) { seen.append(piece); return true; }).run().ok());
    ASSERT_STREQ("hello file", seen.c_str());
    ASSERT_STREQ("hello file", slurp(tmp.at("t.txt")).c_str());
}

UTEST(httpp_client_save_to, save_in_names_the_file_after_the_answer) {
    testutil::local_server ls(serve);
    scratch_dir tmp;
    httpp::response res = httpp::request(ls.url("/named")).save_in(tmp.dir.string()).run();
    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("named body", slurp(tmp.at("named.txt")).c_str());
    ASSERT_STREQ(tmp.at("named.txt").c_str(), res.saved_as.c_str());
}

UTEST(httpp_client_save_to, save_in_falls_back_to_the_name_in_the_url) {
    testutil::local_server ls(serve);
    scratch_dir tmp;
    ASSERT_TRUE(httpp::request(ls.url("/hello")).save_in(tmp.dir.string()).run().ok());
    ASSERT_STREQ("hello file", slurp(tmp.at("hello")).c_str());
}

UTEST(httpp_client_save_to, save_in_has_no_name_when_neither_the_answer_nor_the_url_gives_one) {
    testutil::local_server ls([](httpp::server& s) { s.get("/", [](const httpp::server_request&, httpp::response& res) { res.status = 200; res.body = "root"; }); });
    scratch_dir tmp;
    httpp::response res = httpp::request(ls.url("/")).save_in(tmp.dir.string()).run();
    ASSERT_TRUE(res.error == httpp::error_kind::file);
    ASSERT_NE(std::string::npos, res.error_message.find("no file name"));
}

UTEST(httpp_client_save_to, save_in_never_replaces_a_file_that_is_there_unless_overwrite_is_on) {
    testutil::local_server ls(serve);
    scratch_dir tmp;
    write_file(tmp.at("named.txt"), "mine");
    httpp::response res = httpp::request(ls.url("/named")).save_in(tmp.dir.string()).overwrite(false).run();
    ASSERT_TRUE(res.error == httpp::error_kind::file);
    ASSERT_STREQ("mine", slurp(tmp.at("named.txt")).c_str());
}

// A try that is sent again must not leave its half in the file: it is reopened and truncated when the next head is accepted.

UTEST(httpp_client_save_to, a_retry_after_a_cut_off_body_starts_the_file_again) {
    std::atomic<int> hits{0};
    testutil::local_server ls([&](httpp::server& s) {
        s.get("/drip", [&hits](const httpp::server_request&, httpp::response& res) {
            res.status = 200;
            if (hits++ == 0) {
                res.stream = [](std::size_t, std::string& chunk) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    chunk = "frame\n";
                    return true; // never ends
                };
            } else {
                res.body = "complete";
            }
        });
    });
    scratch_dir tmp;
    httpp::response res = httpp::request(ls.url("/drip")).max_time(1).retry(1).retry_delay(1).save_to(tmp.at("d.txt")).run();
    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("complete", slurp(tmp.at("d.txt")).c_str());
}

UTEST(httpp_client_save_to, a_transient_answer_that_is_sent_again_is_never_written) {
    std::atomic<int> hits{0};
    testutil::local_server ls([&](httpp::server& s) {
        s.get("/flaky", [&hits](const httpp::server_request&, httpp::response& res) {
            if (hits++ == 0) {
                res.status = 503;
                res.body = "oops";
            } else {
                res.status = 200;
                res.body = "fine";
            }
        });
    });
    scratch_dir tmp;
    httpp::response res = httpp::request(ls.url("/flaky")).retry(1).retry_delay(1).save_to(tmp.at("f.txt")).run();
    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("fine", slurp(tmp.at("f.txt")).c_str());
}

UTEST(httpp_client_save_to, run_async_saves_too) {
    testutil::local_server ls(serve);
    scratch_dir tmp;
    httpp::response res = httpp::request(ls.url("/hello")).save_to(tmp.at("as.txt")).run_async().get();
    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("hello file", slurp(tmp.at("as.txt")).c_str());
    ASSERT_STREQ(tmp.at("as.txt").c_str(), res.saved_as.c_str());
}
