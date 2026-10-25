// request::form(): multipart/form-data and urlencoded bodies, parsed only when a handler asks.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <stdexcept>
#include <string>

using testutil::local_server;

namespace {
httpp::request multipart_request(const std::string& body, const std::string& content_type) {
    httpp::request r;
    r.method = "POST";
    r.path = "/";
    r.body = body;
    r.headers.emplace_back("Content-Type", content_type);
    return r;
}
const std::string crlf = "\r\n";
template <class F> bool throws_invalid(F f) {
    try { f(); } catch (const std::invalid_argument&) { return true; } catch (...) { return false; }
    return false;
}
}  // namespace

UTEST(httpp_form, multipart_fields_and_a_binary_file_are_split_exactly) {
    const std::string bin("a\r\n-x\r\nxx\0yy--", 14);  // line breaks, dashes and a NUL (a boundary inside the data would be a bad sender)
    const std::string body =
        "--b" + crlf + "Content-Disposition: form-data; name=\"title\"" + crlf + crlf + "hello world" + crlf +
        "--b" + crlf + "Content-Disposition: form-data; name=\"tag\"" + crlf + crlf + "one" + crlf +
        "--b" + crlf + "Content-Disposition: form-data; name=\"tag\"" + crlf + crlf + "two" + crlf +
        "--b" + crlf + "Content-Disposition: form-data; name=\"doc\"; filename=\"a.bin\"" + crlf +
        "Content-Type: application/x-test" + crlf + crlf + bin + crlf + "--b--" + crlf;
    const auto form = multipart_request(body, "multipart/form-data; boundary=b").form();
    ASSERT_EQ(3u, form.fields.size());
    ASSERT_STREQ("hello world", form.field("title").c_str());
    ASSERT_STREQ("one", form.field("tag").c_str());      // the first of a repeated field
    ASSERT_STREQ("two", form.fields[2].second.c_str());   // ... and all of them are kept, in order
    ASSERT_STREQ("", form.field("missing").c_str());
    ASSERT_EQ(1u, form.files.size());
    const auto* f = form.file("doc");
    ASSERT_TRUE(f != nullptr);
    ASSERT_STREQ("a.bin", f->filename.c_str());
    ASSERT_STREQ("application/x-test", f->content_type.c_str());
    ASSERT_TRUE(f->data == bin);
    ASSERT_TRUE(form.file("title") == nullptr);
}

UTEST(httpp_form, boundary_quotes_preamble_epilogue_header_case_and_filename_cleanup) {
    const std::string body =
        "this is a preamble" + crlf +
        "--" "xYz" + crlf + "content-DISPOSITION: form-data; name=\"f\"; filename=\"C:\\docs\\..\\evil \\\"x\\\".txt\"" + crlf + crlf +
        "data" + crlf + "--xYz--" + crlf + "an epilogue";
    const auto form = multipart_request(body, "Multipart/Form-Data; charset=utf-8; boundary=\"xYz\"").form();
    ASSERT_EQ(1u, form.files.size());
    ASSERT_STREQ("evil \"x\".txt", form.files[0].filename.c_str()); // directories gone, escaped quotes kept
    ASSERT_STREQ("application/octet-stream", form.files[0].content_type.c_str());
    ASSERT_STREQ("data", form.files[0].data.c_str());
}

UTEST(httpp_form, an_empty_file_input_is_a_file_with_an_empty_name) {
    const std::string body = "--b" + crlf + "Content-Disposition: form-data; name=\"up\"; filename=\"\"" + crlf +
                             "Content-Type: application/octet-stream" + crlf + crlf + crlf + "--b--" + crlf;
    const auto form = multipart_request(body, "multipart/form-data; boundary=b").form();
    ASSERT_EQ(1u, form.files.size());
    ASSERT_STREQ("", form.files[0].filename.c_str());
    ASSERT_EQ(0u, form.files[0].data.size());
}

UTEST(httpp_form, urlencoded_bodies_are_decoded_and_keep_repeats) {
    const auto form = multipart_request("a=1&b=x%20y+z&a=2&flag", "application/x-www-form-urlencoded; charset=utf-8").form();
    ASSERT_EQ(4u, form.fields.size());
    ASSERT_STREQ("1", form.field("a").c_str());
    ASSERT_STREQ("x y z", form.field("b").c_str());
    ASSERT_STREQ("2", form.fields[2].second.c_str());
    ASSERT_STREQ("", form.field("flag").c_str());
    ASSERT_TRUE(form.files.empty());
}

UTEST(httpp_form, other_content_types_give_an_empty_form) {
    const auto form = multipart_request("{\"a\":1}", "application/json").form();
    ASSERT_TRUE(form.fields.empty());
    ASSERT_TRUE(form.files.empty());
    httpp::request none;
    none.body = "a=1";
    ASSERT_TRUE(none.form().fields.empty());
}

UTEST(httpp_form, malformed_multipart_throws_invalid_argument) {
    const std::string ok_part = "--b" + crlf + "Content-Disposition: form-data; name=\"a\"" + crlf + crlf + "1" + crlf;
    ASSERT_TRUE(throws_invalid([&] { multipart_request(ok_part + "--b--", "multipart/form-data").form(); }));          // no boundary
    ASSERT_TRUE(throws_invalid([&] { multipart_request(ok_part + "--b--", "multipart/form-data; boundary=").form(); }));
    ASSERT_TRUE(throws_invalid([&] { multipart_request(ok_part, "multipart/form-data; boundary=b").form(); }));         // never closed
    ASSERT_TRUE(throws_invalid([&] { multipart_request("no boundary here", "multipart/form-data; boundary=b").form(); }));
    ASSERT_TRUE(throws_invalid([&] {                                                                                    // no name
        multipart_request("--b" + crlf + "Content-Disposition: form-data" + crlf + crlf + "1" + crlf + "--b--", "multipart/form-data; boundary=b").form();
    }));
    ASSERT_TRUE(throws_invalid([&] {                                                                                    // endless part header
        multipart_request("--b" + crlf + std::string(20000, 'h') + crlf + crlf + "1" + crlf + "--b--", "multipart/form-data; boundary=b").form();
    }));
}

UTEST(httpp_form, a_handler_reads_an_upload_from_a_real_request) {
    local_server srv([](httpp::server& s) {
        s.post("/up", [](const httpp::request& req, httpp::response& res) {
            try {
                const auto form = req.form();
                const auto* f = form.file("doc");
                res.body = form.field("who") + "|" + (f ? f->filename + ":" + std::to_string(f->data.size()) : "none");
            } catch (const std::invalid_argument&) {
                res.status = 400;
                res.body = "bad form";
            }
        });
    });
    const std::string data(100000, 'z');
    const std::string body = "--B" + crlf + "Content-Disposition: form-data; name=\"who\"" + crlf + crlf + "me" + crlf +
                             "--B" + crlf + "Content-Disposition: form-data; name=\"doc\"; filename=\"big.txt\"" + crlf +
                             "Content-Type: text/plain" + crlf + crlf + data + crlf + "--B--" + crlf;
    httpp::client c(srv.url());
    const auto r = c.post("/up", body, "multipart/form-data; boundary=B");
    ASSERT_EQ(200, r.status);
    ASSERT_STREQ("me|big.txt:100000", r.body.c_str());
    ASSERT_EQ(400, c.post("/up", "garbage", "multipart/form-data; boundary=B").status);
}
