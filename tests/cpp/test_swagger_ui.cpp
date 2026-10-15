#include "utest/utest.h"
#include "httpp/swagger_ui.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

// HTTPP_SWAGGER_UI_DIR: src/third_party/swagger-ui, set by tests/cpp/CMakeLists.txt

static std::string read_file(const std::string& name) {
    std::ifstream in(std::string(HTTPP_SWAGGER_UI_DIR) + "/" + name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

UTEST(httpp_swagger_ui, finds_the_three_files_and_nothing_else) {
    for (const char* name : {"swagger-ui-bundle.js", "swagger-ui.css", "favicon-32x32.png"}) {
        const httpp::swagger_ui_asset* a = httpp::find_swagger_ui_asset(name);
        ASSERT_TRUE(a != nullptr);
        EXPECT_TRUE(a->name == name);
    }
    EXPECT_TRUE(httpp::find_swagger_ui_asset("nope.js") == nullptr);
    EXPECT_TRUE(httpp::find_swagger_ui_asset("") == nullptr);
    EXPECT_TRUE(httpp::find_swagger_ui_asset("LICENSE") == nullptr);
}

UTEST(httpp_swagger_ui, compiled_in_bytes_are_identical_to_the_source_files) {
    for (const char* name : {"swagger-ui-bundle.js", "swagger-ui.css", "favicon-32x32.png"}) {
        const std::string on_disk = read_file(name);
        ASSERT_TRUE(!on_disk.empty());
        const httpp::swagger_ui_asset* a = httpp::find_swagger_ui_asset(name);
        ASSERT_TRUE(a != nullptr);
        EXPECT_EQ(a->data.size(), on_disk.size());
        EXPECT_TRUE(a->data == on_disk);  // every byte, including the NULs of the PNG
    }
}

UTEST(httpp_swagger_ui, png_is_binary_safe_and_content_types_are_set) {
    const auto* png = httpp::find_swagger_ui_asset("favicon-32x32.png");
    ASSERT_TRUE(png != nullptr);
    EXPECT_EQ(std::memcmp(png->data.data(), "\x89PNG\r\n\x1a\n", 8), 0);
    EXPECT_TRUE(png->content_type == "image/png");
    EXPECT_TRUE(httpp::find_swagger_ui_asset("swagger-ui.css")->content_type == "text/css");
    EXPECT_TRUE(httpp::find_swagger_ui_asset("swagger-ui-bundle.js")->content_type == "text/javascript");
    EXPECT_TRUE(httpp::find_swagger_ui_asset("swagger-ui-bundle.js")->data.size() > 1000000);
}

UTEST(httpp_swagger_ui, assets_are_stable_for_the_life_of_the_process) {
    EXPECT_TRUE(httpp::find_swagger_ui_asset("swagger-ui.css") == httpp::find_swagger_ui_asset("swagger-ui.css"));
}
