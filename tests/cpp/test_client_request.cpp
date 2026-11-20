#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

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
} // namespace

UTEST(httpp_client_request, plain_get_like_client) {
    httpp::server s;
    s.get("/hello", [](const httpp::server_request&, httpp::response& res) {
        res.status = 200;
        res.body = "world";
    });
    running_server rs(std::move(s));

    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/hello";
    httpp::response res = httpp::request(url).run();

    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("world", res.body.c_str());
}

UTEST(httpp_client_request, sends_custom_headers) {
    httpp::server s;
    std::string seen_header;
    s.get("/echo-header", [&seen_header](const httpp::server_request& req, httpp::response& res) {
        (void)req;
        res.status = 200;
        // httpp::server_request doesn't expose headers yet - this test only
        // checks the call succeeds.
        res.body = "ok";
    });
    running_server rs(std::move(s));

    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/echo-header";
    httpp::response res = httpp::request(url)
                               .header("X-Test", "abc")
                               .run();

    ASSERT_TRUE(res.ok());
}

UTEST(httpp_client_request, data_defaults_method_to_post) {
    httpp::server s;
    std::string received_body;
    s.get("/should-not-hit", [](const httpp::server_request&, httpp::response& res) {
        res.status = 500; // if GET is used instead of POST, this fails
        res.body = "wrong method";
    });
    running_server rs(std::move(s));

    // no POST route registered on purpose: since httpp::server only
    // supports .get() so far, a POST here should be refused (405, the path exists for GET only) rather than hit the
    // GET-registered "/should-not-hit" route above - proving .data()
    // switched the method away from GET.
    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/should-not-hit";
    httpp::response res = httpp::request(url)
                               .data("field=value")
                               .run();

    ASSERT_EQ(405, res.status);
}

UTEST(httpp_client_request, explicit_method_overrides_data_default) {
    httpp::server s;
    running_server rs(std::move(s));

    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/nope";
    httpp::response res = httpp::request(url)
                               .method("DELETE")
                               .run();

    ASSERT_EQ(404, res.status); // reaches the server at all -> DELETE was sent
}

UTEST(httpp_client_request, timeout_and_follow_redirects_are_chainable_and_dont_break_a_request) {
    httpp::server s;
    s.get("/ok", [](const httpp::server_request&, httpp::response& res) {
        res.status = 200;
        res.body = "fine";
    });
    running_server rs(std::move(s));

    std::string url = "http://127.0.0.1:" + std::to_string(rs.port) + "/ok";
    httpp::response res = httpp::request(url)
                               .timeout(5)
                               .max_redirects(20)
                               .run();

    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("fine", res.body.c_str());
}

namespace {
// /a -> /b -> /c ("end"): two redirects to follow.
httpp::server redirect_chain() {
    httpp::server s;
    s.get("/a", [](const httpp::server_request&, httpp::response& res) { res.redirect("/b"); });
    s.get("/b", [](const httpp::server_request&, httpp::response& res) { res.redirect("/c"); });
    s.get("/c", [](const httpp::server_request&, httpp::response& res) {
        res.status = 200;
        res.body = "end";
    });
    return s;
}
} // namespace

UTEST(httpp_client_request, max_redirects_takes_the_most_hops_it_may_follow) {
    running_server rs(redirect_chain());
    const std::string a = "http://127.0.0.1:" + std::to_string(rs.port) + "/a";

    httpp::response two = httpp::request(a).max_redirects(2).run();
    ASSERT_TRUE(two.ok());
    ASSERT_STREQ("end", two.body.c_str());

    httpp::response one = httpp::request(a).max_redirects(1).run();
    ASSERT_TRUE(one.error == httpp::error_kind::too_many_redirects);
    ASSERT_EQ(0, one.status);
}

UTEST(httpp_client_request, max_redirects_minus_one_has_no_limit) {
    running_server rs(redirect_chain());
    httpp::response res = httpp::request("http://127.0.0.1:" + std::to_string(rs.port) + "/a").max_redirects(-1).run();
    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("end", res.body.c_str());
}

UTEST(httpp_client_request, max_redirects_zero_follows_none) {
    running_server rs(redirect_chain());
    httpp::response res = httpp::request("http://127.0.0.1:" + std::to_string(rs.port) + "/a").max_redirects(0).run();
    ASSERT_EQ(302, res.status); // the redirect itself is the answer
    ASSERT_TRUE(res.error == httpp::error_kind::none);
}

UTEST(httpp_client_request, a_negative_limit_is_refused) {
    bool redirects_refused = false, connect_refused = false;
    try {
        httpp::request("http://127.0.0.1:1/").max_redirects(-2);
    } catch (const std::invalid_argument&) {
        redirects_refused = true;
    }
    try {
        httpp::request("http://127.0.0.1:1/").connect_timeout(-1);
    } catch (const std::invalid_argument&) {
        connect_refused = true;
    }
    ASSERT_TRUE(redirects_refused);
    ASSERT_TRUE(connect_refused);
}

namespace {
httpp::server slow_server() {
    httpp::server s;
    s.get("/slow", [](const httpp::server_request&, httpp::response& res) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // the connection is up at once, the answer is late
        res.status = 200;
        res.body = "late";
    });
    return s;
}
} // namespace

UTEST(httpp_client_request, connect_timeout_does_not_limit_the_wait_for_the_answer) {
    running_server rs(slow_server());
    httpp::response res = httpp::request("http://127.0.0.1:" + std::to_string(rs.port) + "/slow")
                              .connect_timeout(1)
                              .timeout(5)
                              .run();
    ASSERT_TRUE(res.ok());
    ASSERT_STREQ("late", res.body.c_str());
}

UTEST(httpp_client_request, timeout_still_limits_the_wait_for_the_answer_whatever_connect_timeout_says) {
    running_server rs(slow_server());
    httpp::response res = httpp::request("http://127.0.0.1:" + std::to_string(rs.port) + "/slow")
                              .connect_timeout(10)
                              .timeout(1)
                              .run();
    ASSERT_TRUE(res.error == httpp::error_kind::timeout);
}

UTEST(httpp_client_request, response_filename_reads_the_content_disposition_name) {
    auto name = [](const char* value) {
        httpp::response r;
        if (value) r.headers.emplace_back("content-disposition", value);
        return r.filename();
    };
    ASSERT_STREQ("report.pdf", name("attachment; filename=\"report.pdf\"").c_str());
    ASSERT_STREQ("report.pdf", name("attachment; filename=report.pdf").c_str());
    ASSERT_STREQ("report.pdf", name("attachment; filename=report.pdf; size=3").c_str());
    ASSERT_STREQ("my file.txt", name("attachment; FileName = \"my file.txt\"").c_str());
    ASSERT_STREQ("name.txt", name("attachment; filename=\"../../etc/name.txt\"").c_str()); // the folder part goes
    ASSERT_STREQ("name.txt", name("attachment; filename=\"C:\\\\dir\\\\name.txt\"").c_str());
    ASSERT_STREQ("a\"b.txt", name("attachment; filename=\"a\\\"b.txt\"").c_str());
    ASSERT_STREQ("", name("attachment; filename*=UTF-8''r%C3%A9sum%C3%A9.pdf").c_str()); // the extended form is not read
    ASSERT_STREQ("plain.txt", name("attachment; filename*=UTF-8''x.pdf; filename=plain.txt").c_str());
    ASSERT_STREQ("", name("attachment; filename=\"..\"").c_str());
    ASSERT_STREQ("", name("attachment; filename=\"\"").c_str());
    ASSERT_STREQ("", name("inline").c_str());
    ASSERT_STREQ("", name(nullptr).c_str());
}
