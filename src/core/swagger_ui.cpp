#include "httpp/swagger_ui.hpp"

#include "httpp_swagger_ui_data.hpp" // generated at configure time, see CMakeLists.txt

#include <iterator>

namespace httpp {

namespace {

std::string_view bytes(const unsigned char* data, std::size_t size) noexcept {
    return {reinterpret_cast<const char*>(data), size};
}

const swagger_ui_asset assets[] = {
    {"swagger-ui-bundle.js", "text/javascript", bytes(embedded::swagger_ui_bundle_js, embedded::swagger_ui_bundle_js_size)},
    {"swagger-ui.css", "text/css", bytes(embedded::swagger_ui_css, embedded::swagger_ui_css_size)},
    {"favicon-32x32.png", "image/png", bytes(embedded::swagger_ui_favicon_png, embedded::swagger_ui_favicon_png_size)},
};

} // namespace

const swagger_ui_asset* find_swagger_ui_asset(std::string_view name) noexcept {
    for (const auto& a : assets)
        if (a.name == name) return &a;
    return nullptr;
}

} // namespace httpp
