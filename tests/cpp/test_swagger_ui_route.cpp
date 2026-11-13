// httpp::swagger_ui: /docs and its three files served from the bytes compiled into the library.
#include "utest/utest.h"
#include "httpp/client.hpp"
#include "httpp/server.hpp"
#include "httpp/swagger_ui.hpp"
#include "local_server.hpp"
#include "swagger_original.hpp"

#include <stdexcept>
#include <string>

using testutil::local_server;

static bool rejects(const std::string& docs_path) {
    httpp::server s;
    try { httpp::swagger_ui().set_docs_path(docs_path).mount(s); } catch (const std::invalid_argument&) { return true; }
    return false;
}

static bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

UTEST(httpp_swagger_ui_route, serves_the_page_and_the_two_files_byte_for_byte) {
    local_server srv([](httpp::server& s) { httpp::swagger_ui().mount(s); });

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
        EXPECT_TRUE(res.body == testutil::swagger_original(*a));  // every byte, as the client sees it
        EXPECT_TRUE(res.header("Content-Type") == a->content_type);
        EXPECT_TRUE(contains(res.header("Cache-Control"), "max-age"));
    }
    EXPECT_EQ(404, httpp::client::fetch(srv.url("/docs/nope.js")).status);

    // the tab icon is the SVG file, served from the library like the rest (nothing from another site)
    EXPECT_TRUE(contains(page.body, "rel=\"icon\" type=\"image/svg+xml\" href=\"/docs/swagger-api-icon.svg\""));
}

UTEST(httpp_swagger_ui_route, custom_path_title_and_openapi_url_are_escaped) {
    local_server srv([](httpp::server& s) {
        httpp::swagger_ui().set_docs_path("/api/docs/").set_openapi_url("/v1/o\"</script>.json").set_title("<b>My & API</b>").mount(s);
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

// ---- how the stored gzip bytes are answered -----------------------------------------------------------------------

static httpp::response get_asset(const local_server& srv, const std::string& accept_encoding, const std::string& name = "swagger-ui-bundle.js") {
    httpp::client c(srv.url());
    return c.get("/docs/" + name, {{"Accept-Encoding", accept_encoding}});
}

UTEST(httpp_swagger_ui_route, a_client_that_takes_gzip_gets_the_stored_bytes_as_they_are) {
    local_server srv([](httpp::server& s) { httpp::swagger_ui().mount(s); });
    const httpp::swagger_ui_asset* a = httpp::find_swagger_ui_asset("swagger-ui-bundle.js");
    auto res = get_asset(srv, "gzip, deflate");
    ASSERT_EQ(200, res.status);
    EXPECT_TRUE(res.body == testutil::swagger_original(*a));
    if (a->gzip) {
        EXPECT_TRUE(contains(res.header("Vary"), "Accept-Encoding"));  // a cache must keep the two versions apart
        EXPECT_TRUE(res.header("Content-Encoding") == "gzip");
        EXPECT_TRUE(res.header("Content-Length") == std::to_string(a->data.size()));  // what went over the wire: the small one
    }
}

UTEST(httpp_swagger_ui_route, a_client_that_does_not_take_gzip_gets_the_original) {
    local_server srv([](httpp::server& s) { httpp::swagger_ui().mount(s); });
    const httpp::swagger_ui_asset* a = httpp::find_swagger_ui_asset("swagger-ui-bundle.js");
    for (const char* accept : {"identity", "gzip;q=0, identity", "br"}) {
        auto res = get_asset(srv, accept);
        ASSERT_EQ(200, res.status);
        EXPECT_TRUE(res.header("Content-Encoding") == "");
        if (a->gzip) EXPECT_TRUE(contains(res.header("Vary"), "Accept-Encoding"));  // with one version only, no Vary is needed
        EXPECT_TRUE(res.header("Content-Length") == std::to_string(a->raw_size));
        EXPECT_TRUE(res.body == testutil::swagger_original(*a));
    }
}

UTEST(httpp_swagger_ui_route, the_small_svg_and_css_follow_the_same_rule) {
    local_server srv([](httpp::server& s) { httpp::swagger_ui().mount(s); });
    for (const char* name : {"swagger-ui.css", "swagger-api-icon.svg"}) {
        const httpp::swagger_ui_asset* a = httpp::find_swagger_ui_asset(name);
        auto plain = get_asset(srv, "identity", name);
        ASSERT_EQ(200, plain.status);
        EXPECT_TRUE(plain.header("Content-Encoding") == "");
        EXPECT_TRUE(plain.body == testutil::swagger_original(*a));
        auto packed = get_asset(srv, "gzip", name);
        ASSERT_EQ(200, packed.status);
        EXPECT_TRUE(packed.body == testutil::swagger_original(*a));
        if (a->gzip) EXPECT_TRUE(packed.header("Content-Encoding") == "gzip");
    }
}

UTEST(httpp_swagger_ui_route, set_compression_does_not_compress_the_stored_bytes_again) {
    local_server srv([](httpp::server& s) {
        s.set_compression(1024);
        httpp::swagger_ui().mount(s);
    });
    const httpp::swagger_ui_asset* a = httpp::find_swagger_ui_asset("swagger-ui-bundle.js");
    auto res = get_asset(srv, "gzip");
    ASSERT_EQ(200, res.status);
    EXPECT_TRUE(res.body == testutil::swagger_original(*a));  // a second gzip layer would leave the first one as the body
    if (a->gzip) EXPECT_TRUE(res.header("Content-Length") == std::to_string(a->data.size()));
}
