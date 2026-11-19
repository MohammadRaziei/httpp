// ADR-0093 and ADR-0094: client and async_client are handles with clone(), the shared settings come from one mixin, and
// client::make_request(path) starts a request from a session.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

using testutil::local_server;

std::string header_of(const httpp::server_request& req, const std::string& name) {
    for (const auto& [k, v] : req.headers) {
        if (k.size() == name.size() &&
            std::equal(k.begin(), k.end(), name.begin(), [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }))
            return v;
    }
    return "";
}

// "METHOD path?query|content-type|authorization|body|X-Default"
void echo_route(httpp::server& s) {
    auto handler = [](const httpp::server_request& req, httpp::response& res) {
        std::string query;
        for (const auto& [k, v] : req.query) query += (query.empty() ? "" : "&") + k + "=" + v;
        res.body = req.method + " " + req.path + "?" + query + "|" + header_of(req, "Content-Type") + "|" +
                   header_of(req, "Authorization") + "|" + req.body + "|" + header_of(req, "X-Default");
    };
    for (const char* m : {"GET", "POST", "PUT", "PATCH", "DELETE"}) {
        s.route(m, "/echo", handler);
        s.route(m, "/api/echo", handler);
    }
}

} // namespace

static_assert(std::is_copy_constructible<httpp::client>::value, "client is a handle (ADR-0093)");
static_assert(std::is_copy_constructible<httpp::async_client>::value, "async_client is a handle (ADR-0093)");
static_assert(!std::is_copy_constructible<httpp::request>::value, "request stays move-only");
static_assert(std::is_move_constructible<httpp::request>::value, "request stays move-only");

UTEST(httpp_client_handle, a_copy_is_the_same_session_and_clone_is_an_independent_one) {
    httpp::client base(std::string("http://127.0.0.1:1/"));
    base.timeout(30).bearer("base-token");

    httpp::client same = base;
    same.timeout(7);
    ASSERT_EQ(7, base.timeout()); // the same session: a change through one handle shows in the other

    httpp::client other = base.clone();
    ASSERT_EQ(7, other.timeout()); // the clone starts with a copy of the settings
    other.timeout(99).max_redirects(4);
    ASSERT_EQ(7, base.timeout());  // and then they are apart, in both directions
    ASSERT_EQ(0, base.max_redirects());
    base.timeout(11);
    ASSERT_EQ(99, other.timeout());
    ASSERT_EQ(4, other.max_redirects());
}

UTEST(httpp_client_handle, a_chain_on_a_temporary_client_keeps_the_session) {
    local_server ls(echo_route);
    auto c = httpp::client(httpp::url(ls.url("/api"))).bearer("tok").timeout(5).max_redirects(2);
    ASSERT_EQ(5, c.timeout());
    ASSERT_EQ(2, c.max_redirects());
    const auto res = c.get("/echo");
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("GET /api/echo?||Bearer tok||", res.body.c_str());
}

UTEST(httpp_client_handle, clone_keeps_the_base_url_and_the_headers_and_credentials) {
    local_server ls(echo_route);
    httpp::client base(httpp::url(ls.url("/api")));
    base.header("X-Default", "d").basic_auth("u", "p");
    httpp::client other = base.clone();
    other.bearer("t"); // basic wins if both are set, so this clone still sends the basic credentials
    const auto res = other.get("/echo");
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("GET /api/echo?||Basic dTpw||d", res.body.c_str());
    other.header("X-Default", "changed");
    ASSERT_STREQ("GET /api/echo?||Basic dTpw||d", base.get("/echo").body.c_str()); // the original did not change
}

UTEST(httpp_client_handle, async_client_copy_is_the_same_session_and_clone_is_independent) {
    local_server ls(echo_route);
    httpp::async_client base(httpp::url(ls.url("/api")), 2);
    base.timeout(30).header("X-Default", "d");

    httpp::async_client same = base;
    same.timeout(8);
    ASSERT_EQ(8, base.timeout());

    httpp::async_client other = base.clone();
    other.timeout(15).bearer("t");
    ASSERT_EQ(8, base.timeout());
    ASSERT_EQ(15, other.timeout());

    const auto res = other.get("/echo").get();
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("GET /api/echo?||Bearer t||d", res.body.c_str());
    ASSERT_STREQ("GET /api/echo?||||d", base.get("/echo").get().body.c_str());
}

UTEST(httpp_client_request, a_request_from_a_session_joins_the_base_path_and_takes_its_settings) {
    local_server ls(echo_route);
    httpp::client c(httpp::url(ls.url("/api")));
    c.bearer("tok").header("X-Default", "d").timeout(9);

    auto r = c.make_request("/echo");
    ASSERT_EQ(9, r.timeout()); // a request has the same getters
    const auto res = r.method("POST").json("{\"a\":1}").run();
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("POST /api/echo?|application/json|Bearer tok|{\"a\":1}|d", res.body.c_str());

    // a path with a query, and a call-level parameter on top of it
    const auto with_query = c.make_request("/echo?x=1").param("y", "2").run();
    ASSERT_EQ(200, with_query.status);
    ASSERT_STREQ("GET /api/echo?x=1&y=2||Bearer tok||d", with_query.body.c_str());
}

UTEST(httpp_client_request, a_request_is_a_snapshot_and_outlives_its_session) {
    local_server ls(echo_route);
    std::unique_ptr<httpp::request> r;
    {
        httpp::client c(httpp::url(ls.url()));
        c.bearer("old");
        r = std::make_unique<httpp::request>(c.make_request("/echo"));
        c.bearer("new").header("X-Default", "later"); // after the request was made: it does not see these
        ASSERT_STREQ("GET /echo?||Bearer new||later", c.get("/echo").body.c_str());
    } // the client is gone
    const auto res = r->run();
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("GET /echo?||Bearer old||", res.body.c_str());
}

UTEST(httpp_client_request, the_request_settings_can_still_be_changed_after_it_started_from_a_session) {
    local_server ls(echo_route);
    httpp::client c(httpp::url(ls.url()));
    c.bearer("session").max_redirects(3);
    auto r = c.make_request("/echo").bearer("call").max_redirects(0);
    ASSERT_EQ(0, r.max_redirects());
    ASSERT_EQ(3, c.max_redirects()); // the session is untouched
    ASSERT_STREQ("GET /echo?||Bearer call||", r.run().body.c_str());
}

UTEST(httpp_client_request, async_client_makes_requests_too) {
    local_server ls(echo_route);
    httpp::async_client a(httpp::url(ls.url("/api")));
    a.bearer("tok");
    const auto res = a.make_request("/echo").param("k", "v").run_async().get();
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("GET /api/echo?k=v||Bearer tok||", res.body.c_str());
}

UTEST(httpp_client_request, an_ipv6_base_url_still_makes_a_usable_url) {
    httpp::client c(std::string("::1"), 1, false);  // nothing listens on port 1: the error must be the connection, not the URL
    const auto res = c.make_request("/x").timeout(2).run();
    ASSERT_EQ(0, res.status);
    ASSERT_TRUE(res.error != httpp::error_kind::invalid_url);
}

UTEST(httpp_client_request, a_chain_on_a_temporary_request_is_a_request) {
    local_server ls(echo_route);
    auto r = httpp::request(httpp::url(ls.url("/echo"))).json("{}").timeout(5).bearer("t").max_redirects(-1).header("X-Default", "h");
    static_assert(std::is_same<decltype(r), httpp::request>::value, "the chain on a temporary yields a request, not a reference");
    ASSERT_EQ(5, r.timeout());
    ASSERT_EQ(-1, r.max_redirects());
    const auto res = r.run();
    ASSERT_EQ(200, res.status);
    ASSERT_STREQ("POST /echo?|application/json|Bearer t|{}|h", res.body.c_str());
}

UTEST(httpp_client_settings, the_rules_are_the_same_in_every_class) {
    httpp::client c(std::string("http://127.0.0.1:1/"));
    httpp::async_client a(httpp::url("http://127.0.0.1:1/"));
    httpp::request r(httpp::url("http://127.0.0.1:1/"));

    ASSERT_EQ(0, c.timeout(-5).timeout());
    ASSERT_EQ(0, a.timeout(-5).timeout());
    ASSERT_EQ(0, r.timeout(-5).timeout());
    ASSERT_EQ(-1, c.max_redirects(-1).max_redirects());
    ASSERT_EQ(-1, a.max_redirects(-1).max_redirects());
    ASSERT_EQ(-1, r.max_redirects(-1).max_redirects());

    auto refused = [](auto& x, auto&& call) {
        bool threw = false;
        try { call(x); } catch (const std::invalid_argument&) { threw = true; }
        return threw;
    };
    ASSERT_TRUE(refused(c, [](auto& x) { x.max_time(-1); }));
    ASSERT_TRUE(refused(a, [](auto& x) { x.max_time(-1); }));
    ASSERT_TRUE(refused(r, [](auto& x) { x.max_time(-1); }));
    ASSERT_TRUE(refused(c, [](auto& x) { x.connect_timeout(-1); }));
    ASSERT_TRUE(refused(a, [](auto& x) { x.connect_timeout(-1); }));
    ASSERT_TRUE(refused(r, [](auto& x) { x.connect_timeout(-1); }));
    ASSERT_TRUE(refused(c, [](auto& x) { x.max_redirects(-2); }));
    ASSERT_TRUE(refused(a, [](auto& x) { x.max_redirects(-2); }));
    ASSERT_TRUE(refused(r, [](auto& x) { x.max_redirects(-2); }));
    ASSERT_TRUE(refused(c, [](auto& x) { x.proxy(httpp::url("host:3128")); }));
    ASSERT_TRUE(refused(a, [](auto& x) { x.proxy(httpp::url("host:3128")); }));
    ASSERT_TRUE(refused(r, [](auto& x) { x.proxy(httpp::url("host:3128")); }));

    ASSERT_TRUE(c.auto_compression() && a.auto_compression() && r.auto_compression());
    ASSERT_FALSE(c.auto_compression(false).auto_compression());
    ASSERT_FALSE(a.auto_compression(false).auto_compression());
    ASSERT_FALSE(r.auto_compression(false).auto_compression());
    ASSERT_STREQ("x.pem", c.ca_file("x.pem").ca_file().str().c_str());
    ASSERT_STREQ("x.pem", a.ca_file("x.pem").ca_file().str().c_str());
    ASSERT_STREQ("x.pem", r.ca_file("x.pem").ca_file().str().c_str());
}
