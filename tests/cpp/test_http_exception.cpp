// httpp::http_exception: the C++ twin of Python's HTTPException (ADR-0020).
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <exception>
#include <stdexcept>
#include <string>

using testutil::local_server;

UTEST(httpp_http_exception, carries_status_detail_and_headers) {
    httpp::http_exception e(401, "no token", {{"WWW-Authenticate", "Bearer"}});
    ASSERT_EQ(401, e.status());
    ASSERT_STREQ("no token", e.detail().c_str());
    ASSERT_EQ(1u, e.headers().size());
    ASSERT_STREQ("WWW-Authenticate", e.headers()[0].first.c_str());
    ASSERT_STREQ("401: no token", e.what()); // the same text as Python's str(HTTPException)
}

UTEST(httpp_http_exception, detail_defaults_to_the_reason_phrase) {
    ASSERT_STREQ("Not Found", httpp::http_exception(404).detail().c_str());
    ASSERT_STREQ("Unprocessable Entity", httpp::http_exception(422).detail().c_str());
    ASSERT_STREQ("", httpp::http_exception(299).detail().c_str()); // no phrase for it: empty, as in Python
}

UTEST(httpp_http_exception, a_status_outside_100_to_599_is_refused) {
    for (const int bad : {0, 99, 600, -404}) {
        bool refused = false;
        try { httpp::http_exception e(bad); } catch (const std::invalid_argument&) { refused = true; }
        ASSERT_TRUE(refused);
    }
}

UTEST(httpp_http_exception, a_throwing_handler_answers_with_the_detail_as_json) {
    local_server srv([](httpp::server& s) {
        s.get("/users/{id}", [](const httpp::server_request&, httpp::response&) {
            throw httpp::http_exception(404, "no such user");
        });
        s.get("/admin", [](const httpp::server_request&, httpp::response&) {
            throw httpp::http_exception(401, "no token", {{"WWW-Authenticate", "Bearer"}});
        });
        s.get("/plain", [](const httpp::server_request&, httpp::response&) { throw httpp::http_exception(410); });
        s.get("/quote", [](const httpp::server_request&, httpp::response&) {
            throw httpp::http_exception(400, "say \"hi\"\\\n"); // the detail is escaped, so the answer stays valid JSON
        });
    });
    auto nf = httpp::client::fetch(srv.url("/users/9"));
    ASSERT_EQ(404, nf.status);
    ASSERT_STREQ("{\"detail\":\"no such user\"}", nf.body.c_str());
    ASSERT_TRUE(nf.header("Content-Type").rfind("application/json", 0) == 0);

    auto admin = httpp::client::fetch(srv.url("/admin"));
    ASSERT_EQ(401, admin.status);
    ASSERT_STREQ("Bearer", admin.header("WWW-Authenticate").c_str());

    auto plain = httpp::client::fetch(srv.url("/plain"));
    ASSERT_EQ(410, plain.status);
    ASSERT_STREQ("{\"detail\":\"Gone\"}", plain.body.c_str());

    auto quote = httpp::client::fetch(srv.url("/quote"));
    ASSERT_EQ(400, quote.status);
    ASSERT_STREQ("{\"detail\":\"say \\\"hi\\\"\\\\\\n\"}", quote.body.c_str());
}

UTEST(httpp_http_exception, an_async_handler_may_throw_it_too) {
    local_server srv([](httpp::server& s) {
        s.get_async("/a", [](const httpp::server_request&, httpp::responder) { throw httpp::http_exception(403, "nope"); });
    });
    auto r = httpp::client::fetch(srv.url("/a"));
    ASSERT_EQ(403, r.status);
    ASSERT_STREQ("{\"detail\":\"nope\"}", r.body.c_str());
}

UTEST(httpp_http_exception, the_exception_handler_sees_it_and_may_change_the_answer) {
    local_server srv([](httpp::server& s) {
        s.exception_handler([](const httpp::server_request&, httpp::response& res, std::exception_ptr error) {
            try {
                std::rethrow_exception(error);
            } catch (const httpp::http_exception& e) {
                if (e.status() == 404) res.body = "custom " + e.detail(); // changes the body, keeps the status
            } catch (...) {
            }
        });
        s.get("/missing", [](const httpp::server_request&, httpp::response&) { throw httpp::http_exception(404, "gone"); });
        s.get("/left", [](const httpp::server_request&, httpp::response&) { throw httpp::http_exception(409, "left alone"); });
        s.get("/other", [](const httpp::server_request&, httpp::response&) { throw std::runtime_error("x"); });
    });
    auto missing = httpp::client::fetch(srv.url("/missing"));
    ASSERT_EQ(404, missing.status);
    ASSERT_STREQ("custom gone", missing.body.c_str());
    auto left = httpp::client::fetch(srv.url("/left")); // the handler ignored it: the default answer stays
    ASSERT_EQ(409, left.status);
    ASSERT_STREQ("{\"detail\":\"left alone\"}", left.body.c_str());
    ASSERT_EQ(500, httpp::client::fetch(srv.url("/other")).status); // any other exception is still a 500
}
