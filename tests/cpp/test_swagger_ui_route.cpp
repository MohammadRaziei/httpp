// httpp::mount_swagger_ui: /docs and its three files served from the bytes compiled into the library.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "httpp/swagger_ui.hpp"
#include "local_server.hpp"

#include <stdexcept>
#include <string>

using testutil::local_server;

static bool rejects(const std::string& docs_path) {
    httpp::server s;
    try { httpp::mount_swagger_ui(s, docs_path); } catch (const std::invalid_argument&) { return true; }
    return false;
}

static bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

UTEST(httpp_swagger_ui_route, serves_the_page_and_the_two_files_byte_for_byte) {
    local_server srv([](httpp::server& s) { httpp::mount_swagger_ui(s); });

    auto page = httpp::client::fetch(srv.url("/docs"));
    ASSERT_EQ(200, page.status);
    EXPECT_TRUE(contains(page.header("Content-Type"), "text/html"));
    EXPECT_TRUE(contains(page.body, "/docs/swagger-ui-bundle.js"));
    EXPECT_TRUE(contains(page.body, "\"/openapi.json\""));  // JSON string literal, so it is a valid JS string

    for (const char* name : {"swagger-ui-bundle.js", "swagger-ui.css", "swagger-api-icon.svg"}) {
        auto res = httpp::client::fetch(srv.url(std::string("/docs/") + name));
        ASSERT_EQ(200, res.status);
        const httpp::swagger_ui_asset* a = httpp::find_swagger_ui_asset(name);
        ASSERT_TRUE(a != nullptr);
        EXPECT_TRUE(res.body == a->data);  // every byte
        EXPECT_TRUE(res.header("Content-Type") == a->content_type);
        EXPECT_TRUE(contains(res.header("Cache-Control"), "max-age"));
    }
    EXPECT_EQ(404, httpp::client::fetch(srv.url("/docs/nope.js")).status);

    // the tab icon is the SVG file, served from the library like the rest (nothing from another site)
    EXPECT_TRUE(contains(page.body, "rel=\"icon\" type=\"image/svg+xml\" href=\"/docs/swagger-api-icon.svg\""));
}

UTEST(httpp_swagger_ui_route, custom_path_title_and_openapi_url_are_escaped) {
    local_server srv([](httpp::server& s) {
        httpp::mount_swagger_ui(s, "/api/docs/", "/v1/o\"</script>.json", "<b>My & API</b>");
    });
    auto page = httpp::client::fetch(srv.url("/api/docs/"));
    ASSERT_EQ(200, page.status);
    EXPECT_TRUE(contains(page.body, "&lt;b&gt;My &amp; API&lt;/b&gt;"));
    EXPECT_FALSE(contains(page.body, "<b>My"));
    EXPECT_FALSE(contains(page.body, "</script>.json"));  // cannot close the inline script early
    EXPECT_TRUE(contains(page.body, "/api/docs/swagger-ui.css"));  // trailing slash does not double up
    EXPECT_EQ(200, httpp::client::fetch(srv.url("/api/docs/swagger-ui.css")).status);
}

UTEST(httpp_swagger_ui_route, bad_docs_path_is_rejected) {
    EXPECT_TRUE(rejects(""));
    EXPECT_TRUE(rejects("docs"));
    EXPECT_TRUE(rejects("/"));
}
