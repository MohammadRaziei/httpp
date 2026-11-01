#include "utest/utest.h"
#include "httpp/swagger_ui.hpp"
#include "swagger_original.hpp"

#include <fstream>
#include <iterator>
#include <string>

// HTTPP_SWAGGER_UI_DIR: src/third_party/swagger-ui, set by tests/cpp/CMakeLists.txt
// HTTPP_EXPECT_EMBED_GZIP: 1 when the library was built with -DHTTPP_EMBED_COMPRESS=ON (the default), else 0

static std::string read_file(const std::string& name) {
    std::ifstream in(std::string(HTTPP_SWAGGER_UI_DIR) + "/" + name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

UTEST(httpp_swagger_ui, finds_the_three_files_and_nothing_else) {
    for (const char* name : {"swagger-ui-bundle.js", "swagger-ui.css", "swagger-api-icon.svg"}) {
        const httpp::swagger_ui_asset* a = httpp::find_swagger_ui_asset(name);
        ASSERT_TRUE(a != nullptr);
        EXPECT_TRUE(a->name == name);
    }
    EXPECT_TRUE(httpp::find_swagger_ui_asset("nope.js") == nullptr);
    EXPECT_TRUE(httpp::find_swagger_ui_asset("") == nullptr);
    EXPECT_TRUE(httpp::find_swagger_ui_asset("LICENSE") == nullptr);
}

UTEST(httpp_swagger_ui, compiled_in_bytes_are_identical_to_the_source_files) {
    for (const char* name : {"swagger-ui-bundle.js", "swagger-ui.css", "swagger-api-icon.svg"}) {
        const std::string on_disk = read_file(name);
        ASSERT_TRUE(!on_disk.empty());
        const httpp::swagger_ui_asset* a = httpp::find_swagger_ui_asset(name);
        ASSERT_TRUE(a != nullptr);
        EXPECT_EQ(a->raw_size, on_disk.size());
        EXPECT_TRUE(testutil::swagger_original(*a) == on_disk);  // every byte, after undoing the compression
    }
}

UTEST(httpp_swagger_ui, content_types_are_set) {
    EXPECT_TRUE(httpp::find_swagger_ui_asset("swagger-ui.css")->content_type == "text/css");
    EXPECT_TRUE(httpp::find_swagger_ui_asset("swagger-ui-bundle.js")->content_type == "text/javascript");
    EXPECT_TRUE(httpp::find_swagger_ui_asset("swagger-api-icon.svg")->content_type == "image/svg+xml");
    EXPECT_TRUE(httpp::find_swagger_ui_asset("swagger-ui-bundle.js")->raw_size > 1000000);
}

UTEST(httpp_swagger_ui, stored_bytes_are_gzip_when_the_build_compresses) {
    for (const char* name : {"swagger-ui-bundle.js", "swagger-ui.css", "swagger-api-icon.svg"}) {
        const httpp::swagger_ui_asset* a = httpp::find_swagger_ui_asset(name);
        ASSERT_TRUE(a != nullptr);
        EXPECT_EQ(a->gzip, HTTPP_EXPECT_EMBED_GZIP != 0);
        if (a->gzip) {
            ASSERT_TRUE(a->data.size() > 18);
            EXPECT_TRUE(static_cast<unsigned char>(a->data[0]) == 0x1f && static_cast<unsigned char>(a->data[1]) == 0x8b);
            EXPECT_TRUE(a->data.size() < a->raw_size);
        } else {
            EXPECT_EQ(a->data.size(), a->raw_size);
        }
    }
    const httpp::swagger_ui_asset* js = httpp::find_swagger_ui_asset("swagger-ui-bundle.js");
    if (js->gzip) EXPECT_TRUE(js->data.size() < js->raw_size / 2);  // 1.5 MB of minified JS packs to about a quarter
}

UTEST(httpp_swagger_ui, assets_are_stable_for_the_life_of_the_process) {
    EXPECT_TRUE(httpp::find_swagger_ui_asset("swagger-ui.css") == httpp::find_swagger_ui_asset("swagger-ui.css"));
}
