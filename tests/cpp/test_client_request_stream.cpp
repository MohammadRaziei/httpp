#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <atomic>
#include <string>
#include <string_view>

// request::on_head and request::on_body: the body goes to a callback as it arrives (ADR-0084).

UTEST(httpp_client_request_stream, the_body_goes_to_the_callback_and_not_to_the_response) {
    testutil::local_server ls([](httpp::server& s) {
        s.get("/big", [](const httpp::server_request&, httpp::response& res) {
            res.status = 200;
            res.body = std::string(300000, 'x');
        });
    });
    std::string got;
    int chunks = 0;
    httpp::response res = httpp::request(ls.url("/big"))
                              .on_body([&](std::string_view piece) {
                                  got.append(piece);
                                  ++chunks;
                                  return true;
                              })
                              .run();
    ASSERT_TRUE(res.ok());
    ASSERT_TRUE(res.body.empty());
    ASSERT_EQ(got.size(), 300000u);
    ASSERT_TRUE(got == std::string(300000, 'x'));
    ASSERT_GT(chunks, 1); // as it arrives, not in one piece
}

UTEST(httpp_client_request_stream, on_head_sees_the_status_and_headers_before_the_first_piece) {
    testutil::local_server ls([](httpp::server& s) {
        s.get("/h", [](const httpp::server_request&, httpp::response& res) {
            res.status = 201;
            res.headers.emplace_back("X-Test", "yes");
            res.body = "abc";
        });
    });
    int status = 0;
    std::string header;
    bool head_came_first = true;
    bool head_seen = false;
    std::string got;
    httpp::response res = httpp::request(ls.url("/h"))
                              .on_head([&](const httpp::response& head) {
                                  head_seen = true;
                                  status = head.status;
                                  header = head.header("X-Test");
                                  return true;
                              })
                              .on_body([&](std::string_view piece) {
                                  if (!head_seen) head_came_first = false;
                                  got.append(piece);
                                  return true;
                              })
                              .run();
    ASSERT_EQ(status, 201);
    ASSERT_STREQ("yes", header.c_str());
    ASSERT_TRUE(head_came_first);
    ASSERT_STREQ("abc", got.c_str());
    ASSERT_EQ(res.status, 201);
}

UTEST(httpp_client_request_stream, on_head_returning_false_skips_the_body_but_the_response_still_comes_back) {
    testutil::local_server ls([](httpp::server& s) {
        s.get("/missing", [](const httpp::server_request&, httpp::response& res) {
            res.status = 404;
            res.body = "an error page";
        });
    });
    int pieces = 0;
    httpp::response res = httpp::request(ls.url("/missing"))
                              .on_head([](const httpp::response& head) { return head.status < 400; })
                              .on_body([&](std::string_view) {
                                  ++pieces;
                                  return true;
                              })
                              .run();
    ASSERT_EQ(res.status, 404);
    ASSERT_FALSE(res.failed());
    ASSERT_EQ(pieces, 0);
    ASSERT_TRUE(res.body.empty());
}

UTEST(httpp_client_request_stream, on_body_returning_false_stops_an_endless_answer) {
    testutil::local_server ls([](httpp::server& s) {
        s.get("/endless", [](const httpp::server_request&, httpp::response& res) {
            res.status = 200;
            res.stream = [](std::size_t, std::string& chunk) {
                chunk = std::string(4096, 'z');
                return true; // never ends
            };
        });
    });
    std::size_t total = 0;
    httpp::response res = httpp::request(ls.url("/endless"))
                              .timeout(10)
                              .on_body([&](std::string_view piece) {
                                  total += piece.size();
                                  return total < 20000;
                              })
                              .run();
    ASSERT_STREQ("canceled", res.error_name());
    ASSERT_GE(total, 20000u);
}

UTEST(httpp_client_request_stream, a_streaming_request_asks_for_identity_unless_a_header_says_otherwise) {
    std::string seen;
    testutil::local_server ls([&seen](httpp::server& s) {
        s.get("/enc", [&seen](const httpp::server_request& req, httpp::response& res) {
            seen = req.header("Accept-Encoding");
            res.status = 200;
            res.body = "ok";
        });
    });
    auto sink = [](std::string_view) { return true; };
    httpp::request(ls.url("/enc")).on_body(sink).run();
    ASSERT_STREQ("identity", seen.c_str());
    httpp::request(ls.url("/enc")).header("Accept-Encoding", "gzip").on_body(sink).run();
    ASSERT_STREQ("gzip", seen.c_str());
}

UTEST(httpp_client_request_stream, a_followed_redirect_is_not_offered_to_the_callbacks) {
    testutil::local_server ls([](httpp::server& s) {
        s.get("/old", [](const httpp::server_request&, httpp::response& res) {
            res.redirect("/new");
            res.body = "moved";
        });
        s.get("/new", [](const httpp::server_request&, httpp::response& res) {
            res.status = 200;
            res.body = "the real body";
        });
    });
    std::string got;
    int head_calls = 0;
    int head_status = 0;
    httpp::response res = httpp::request(ls.url("/old"))
                              .follow_redirects(true)
                              .on_head([&](const httpp::response& head) {
                                  ++head_calls;
                                  head_status = head.status;
                                  return true;
                              })
                              .on_body([&](std::string_view piece) {
                                  got.append(piece);
                                  return true;
                              })
                              .run();
    ASSERT_EQ(res.status, 200);
    ASSERT_EQ(head_calls, 1);
    ASSERT_EQ(head_status, 200);
    ASSERT_STREQ("the real body", got.c_str());
}

UTEST(httpp_client_request_stream, without_follow_redirects_the_redirect_is_the_answer_and_its_body_is_streamed) {
    testutil::local_server ls([](httpp::server& s) {
        s.get("/old", [](const httpp::server_request&, httpp::response& res) {
            res.redirect("/new");
            res.body = "moved";
        });
    });
    std::string got;
    httpp::response res = httpp::request(ls.url("/old"))
                              .on_body([&](std::string_view piece) {
                                  got.append(piece);
                                  return true;
                              })
                              .run();
    ASSERT_EQ(res.status, 302);
    ASSERT_STREQ("moved", got.c_str());
}
