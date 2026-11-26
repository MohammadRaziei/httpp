// ADR-0113: request::field(), file(), file_data(), body_file() and progress().
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

namespace {

using testutil::local_server;

// "fields|files|content-length" where fields is name=value;... and files is field:filename:type:size:first-and-last-byte;...
void upload_route(httpp::server& s) {
    s.post("/upload", [](const httpp::server_request& req, httpp::response& res) {
        const httpp::form_data form = req.form();
        std::string out;
        for (const auto& [k, v] : form.fields) out += k + "=" + v + ";";
        out += "|";
        for (const auto& f : form.files) {
            out += f.field + ":" + f.filename + ":" + f.content_type + ":" + std::to_string(f.data.size());
            if (!f.data.empty()) out += std::string(":") + f.data.front() + f.data.back();
            out += ";";
        }
        out += "|" + req.header("Content-Length");
        res.body = out;
    });
    s.put("/store", [](const httpp::server_request& req, httpp::response& res) {
        res.body = std::to_string(req.body.size()) + "|" + req.header("Content-Type") + "|" + req.header("Content-Length") + "|" +
                   (req.body.empty() ? std::string() : std::string(1, req.body.front()) + req.body.back());
    });
    s.get("/moved", [](const httpp::server_request&, httpp::response& res) {
        res.status = 303;
        res.headers.emplace_back("Location", "/done");
    });
    s.post("/moved", [](const httpp::server_request&, httpp::response& res) {
        res.status = 303;
        res.headers.emplace_back("Location", "/done");
    });
    s.get("/done", [](const httpp::server_request& req, httpp::response& res) { res.body = req.method + ":" + std::to_string(req.body.size()); });
}

// A file of `size` bytes: 'a', then 'm' ..., ending in 'z'.
std::string make_file(const std::string& name, std::size_t size) {
    std::string bytes(size, 'm');
    if (size) {
        bytes.front() = 'a';
        bytes.back() = 'z';
    }
    std::ofstream(name, std::ios::binary) << bytes;
    return name;
}

} // namespace

UTEST(httpp_client_upload, fields_and_a_file_arrive_as_a_form) {
    local_server ls(upload_route);
    const std::string file = make_file("upload_small.txt", 10);
    const auto res = httpp::request(httpp::url(ls.url("/upload"))).field("title", "hi there").file("doc", httpp::path(file)).run();
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("title=hi there;|doc:upload_small.txt:text/plain:10:az;|", res.body.substr(0, res.body.rfind('|') + 1).c_str());
    std::remove(file.c_str());
}

UTEST(httpp_client_upload, a_file_name_and_type_can_be_given_and_data_needs_no_file) {
    local_server ls(upload_route);
    const std::string file = make_file("upload_named.bin", 4);
    const auto res = httpp::request(httpp::url(ls.url("/upload")))
                         .file("one", httpp::path(file), "photo.png", "image/png")
                         .file_data("two", "xyz", "notes.txt", "text/plain")
                         .file_data("three", "q", "blob")
                         .run();
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("|one:photo.png:image/png:4:az;two:notes.txt:text/plain:3:xz;three:blob:application/octet-stream:1:qq;|",
                 res.body.substr(0, res.body.rfind('|') + 1).c_str());
    std::remove(file.c_str());
}

UTEST(httpp_client_upload, a_quote_in_a_name_survives) {
    local_server ls(upload_route);
    const auto res = httpp::request(httpp::url(ls.url("/upload"))).file_data("f", "x", "a\"b.txt").run();
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("|f:a\"b.txt:text/plain:1:xx;|", res.body.substr(0, res.body.rfind('|') + 1).c_str());
}

UTEST(httpp_client_upload, a_big_file_is_sent_in_pieces_and_progress_reaches_the_total) {
    local_server ls(upload_route);
    const std::size_t size = 300 * 1000;
    const std::string file = make_file("upload_big.bin", size);
    std::mutex m;
    std::uint64_t last = 0, total_seen = 0;
    int calls = 0;
    const auto res = httpp::request(httpp::url(ls.url("/upload")))
                         .field("k", "v")
                         .file("big", httpp::path(file))
                         .progress([&](std::uint64_t sent, std::uint64_t total) {
                             std::lock_guard<std::mutex> lock(m);
                             ASSERT_GE(sent, 0u);
                             last = sent;
                             total_seen = total;
                             ++calls;
                         })
                         .run();
    ASSERT_EQ(200, res.status);
    ASSERT_NE(std::string::npos, res.body.find("big:upload_big.bin:application/octet-stream:300000:az;"));
    std::lock_guard<std::mutex> lock(m);
    ASSERT_EQ(total_seen, last); // the last call says everything was sent
    ASSERT_GT(total_seen, size);  // the total is the whole body: the file and the form around it
    ASSERT_GT(calls, 3);          // several pieces, not one write
    std::remove(file.c_str());
}

UTEST(httpp_client_upload, a_missing_file_is_an_error_and_nothing_is_sent) {
    local_server ls(upload_route);
    const auto res = httpp::request(httpp::url(ls.url("/upload"))).file("doc", httpp::path("no_such_file_here.bin")).run();
    ASSERT_EQ(0, res.status);
    ASSERT_TRUE(res.error == httpp::error_kind::file);
    ASSERT_NE(std::string::npos, res.error_message.find("no_such_file_here.bin"));
}

UTEST(httpp_client_upload, mixing_kinds_of_body_is_an_error) {
    local_server ls(upload_route);
    const auto a = httpp::request(httpp::url(ls.url("/upload"))).data("x=1").field("k", "v").run();
    ASSERT_TRUE(a.error == httpp::error_kind::file);
    const std::string file = make_file("upload_mix.bin", 2);
    const auto b = httpp::request(httpp::url(ls.url("/upload"))).field("k", "v").body_file(httpp::path(file)).run();
    ASSERT_TRUE(b.error == httpp::error_kind::file);
    std::remove(file.c_str());
}

UTEST(httpp_client_upload, body_file_puts_the_whole_file_with_its_type) {
    local_server ls(upload_route);
    const std::string file = make_file("upload_whole.json", 250 * 1000);
    const auto res = httpp::request(httpp::url(ls.url("/store"))).body_file(httpp::path(file)).run();
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("250000|application/json|250000|az", res.body.c_str());
    const auto typed = httpp::request(httpp::url(ls.url("/store"))).body_file(httpp::path(file)).content_type("text/x-mine").run();
    ASSERT_NE(std::string::npos, typed.body.find("|text/x-mine|"));
    std::remove(file.c_str());
}

UTEST(httpp_client_upload, an_empty_file_is_sent_with_length_zero) {
    local_server ls(upload_route);
    const std::string file = make_file("upload_empty.txt", 0);
    const auto res = httpp::request(httpp::url(ls.url("/store"))).body_file(httpp::path(file)).run();
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("0|text/plain|0|", res.body.c_str());
    std::remove(file.c_str());
}

UTEST(httpp_client_upload, a_303_answer_is_followed_with_a_get_and_no_body) {
    local_server ls(upload_route);
    const std::string file = make_file("upload_redirect.bin", 5000);
    const auto res = httpp::request(httpp::url(ls.url("/moved"))).file("f", httpp::path(file)).max_redirects(3).run();
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("GET:0", res.body.c_str());
    std::remove(file.c_str());
}

UTEST(httpp_client_upload, the_method_can_be_chosen_and_run_async_works) {
    local_server ls(upload_route);
    const std::string file = make_file("upload_async.bin", 100);
    auto future = httpp::request(httpp::url(ls.url("/store"))).method("PUT").body_file(httpp::path(file)).run_async();
    const auto res = future.get();
    ASSERT_EQ(200, res.status);
    ASSERT_NE(std::string::npos, res.body.find("100|"));
    std::remove(file.c_str());
}

UTEST(httpp_client_upload, a_clone_carries_the_parts_and_the_progress_callback) {
    local_server ls(upload_route);
    int calls = 0;
    auto base = httpp::request(httpp::url(ls.url("/upload"))).field("k", "v").file_data("d", "abc", "a.txt").progress([&](std::uint64_t, std::uint64_t) { ++calls; });
    auto copy = base.clone();
    const auto res = copy.run();
    ASSERT_EQ(200, res.status);
    ASSERT_NE(std::string::npos, res.body.find("k=v;|d:a.txt:text/plain:3:ac;"));
    ASSERT_GT(calls, 0);
}
