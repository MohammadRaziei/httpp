// httpp::router and server::include: prefixes are joined once, at registration.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/router.hpp"
#include "httpp/server.hpp"
#include "local_server.hpp"

#include <memory>
#include <stdexcept>
#include <string>

using testutil::local_server;

namespace {
httpp::handler say(const std::string& text) {
    return [text](const httpp::request&, httpp::response& res) { res.body = text; };
}
int status_of(const local_server& s, const std::string& path) { return httpp::client::fetch(s.url(path)).status; }
std::string body_of(const local_server& s, const std::string& path) { return httpp::client::fetch(s.url(path)).body; }
}  // namespace

UTEST(httpp_router, a_prefix_is_put_in_front_of_every_path) {
    local_server srv([](httpp::server& s) {
        httpp::router r("/users");
        r.get("/", say("list")).get("/:id", [](const httpp::request& req, httpp::response& res) { res.body = "user " + req.path_param("id"); });
        r.post("/new", say("created"));
        s.include(r);
    });
    ASSERT_STREQ("list", body_of(srv, "/users").c_str());  // "/" under a prefix is the prefix itself
    ASSERT_STREQ("user 7", body_of(srv, "/users/7").c_str());
    ASSERT_EQ(404, status_of(srv, "/7"));
    ASSERT_STREQ("user new", body_of(srv, "/users/new").c_str()); // GET: the first route that fits (":id") wins, as without a router
}

UTEST(httpp_router, include_adds_an_outer_prefix_and_routers_nest) {
    local_server srv([](httpp::server& s) {
        httpp::router users("/users");
        users.get("/me", say("me"));
        httpp::router api("api");        // no leading slash, still "/api"
        api.include(users);              // /api/users/me
        api.get("/ping", say("pong"));
        httpp::router no_prefix;
        no_prefix.get("/plain", say("plain"));
        s.include(api, "/v1/");          // trailing slash is dropped: /v1/api/...
        s.include(no_prefix);
    });
    ASSERT_STREQ("me", body_of(srv, "/v1/api/users/me").c_str());
    ASSERT_STREQ("pong", body_of(srv, "/v1/api/ping").c_str());
    ASSERT_STREQ("plain", body_of(srv, "/plain").c_str());
    ASSERT_EQ(404, status_of(srv, "/api/ping"));
}

UTEST(httpp_router, a_router_can_be_included_twice_and_changed_or_dropped_afterwards) {
    local_server srv([](httpp::server& s) {
        auto r = std::make_unique<httpp::router>("/x");
        r->get("/a", say("a"));
        s.include(*r, "/one");
        s.include(*r, "/two");
        r->get("/late", say("late"));    // added after include: not on the server
        r.reset();
    });
    ASSERT_STREQ("a", body_of(srv, "/one/x/a").c_str());
    ASSERT_STREQ("a", body_of(srv, "/two/x/a").c_str());
    ASSERT_EQ(404, status_of(srv, "/one/x/late"));
}

UTEST(httpp_router, async_routes_work_through_a_router_too) {
    local_server srv([](httpp::server& s) {
        httpp::router r("/a");
        r.get_async("/x", [](const httpp::request&, httpp::responder rp) {
            httpp::response res;
            res.body = "async";
            rp.send(res);
        });
        s.include(r);
    });
    ASSERT_STREQ("async", body_of(srv, "/a/x").c_str());
}

UTEST(httpp_router, an_unsupported_method_is_refused_when_the_router_is_included) {
    httpp::router r;
    r.route("FETCH", "/x", say("x"));  // not checked yet
    httpp::server s;
    bool threw = false;
    try { s.include(r); } catch (const std::invalid_argument&) { threw = true; }
    ASSERT_TRUE(threw);
}

UTEST(httpp_router, prefix_and_routes_can_be_read_back) {
    httpp::router r("/p/");
    r.get("x", say("x"));
    ASSERT_STREQ("/p", r.prefix().c_str());
    ASSERT_EQ(1u, r.routes().size());
    ASSERT_STREQ("/x", r.routes()[0].path.c_str());
    ASSERT_STREQ("GET", r.routes()[0].method.c_str());
}
