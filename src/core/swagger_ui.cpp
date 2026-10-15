#include "httpp/swagger_ui.hpp"

#include "httpp_swagger_ui_data.hpp" // generated at configure time, see CMakeLists.txt

#include <iterator>
#include <stdexcept>
#include <string>

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

std::string html_escape(std::string_view in) {
    std::string out;
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

// A JS string literal that is also safe inside an inline <script>: "</" and "<!--" cannot close or confuse it.
std::string js_string(std::string_view in) {
    std::string out = "\"";
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '<': out += "\\u003c"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out + "\"";
}

} // namespace

const swagger_ui_asset* find_swagger_ui_asset(std::string_view name) noexcept {
    for (const auto& a : assets)
        if (a.name == name) return &a;
    return nullptr;
}

void mount_swagger_ui(server& s, const std::string& docs_path, const std::string& openapi_url, const std::string& title) {
    std::string base = docs_path;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (docs_path.empty() || docs_path[0] != '/' || base.empty())
        throw std::invalid_argument("mount_swagger_ui: docs_path must start with '/' and be more than the root");

    const std::string safe_base = html_escape(base);
    const std::string page =
        "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>" + html_escape(title) +
        " - Swagger UI</title>\n<link rel=\"icon\" type=\"image/png\" href=\"" + safe_base + "/favicon-32x32.png\">\n"
        "<link rel=\"stylesheet\" href=\"" + safe_base + "/swagger-ui.css\">\n</head>\n<body>\n<div id=\"swagger-ui\"></div>\n"
        "<script src=\"" + safe_base + "/swagger-ui-bundle.js\"></script>\n<script>\n"
        "window.ui = SwaggerUIBundle({url: " + js_string(openapi_url) + ", dom_id: \"#swagger-ui\", layout: \"BaseLayout\", "
        "deepLinking: true,\n  presets: [SwaggerUIBundle.presets.apis]});\n</script>\n</body>\n</html>\n";

    // The page answers both "/docs" and "/docs/": people type either.
    for (const std::string& path : {base, base + "/"}) {
        s.get(path, [page](const request&, response& res) {
            res.status = 200;
            res.headers.emplace_back("Content-Type", "text/html; charset=utf-8");
            res.body = page;
        });
    }
    for (const auto& a : assets) {
        // ponytail: res.body is a std::string, so each request copies the file (the bundle is ~1.5 MB). Fine for a
        // page opened by hand; the upgrade is a string_view body in response, which the engine would send as is.
        s.get(base + "/" + std::string(a.name), [&a](const request&, response& res) {
            res.status = 200;
            res.headers.emplace_back("Content-Type", std::string(a.content_type));
            res.headers.emplace_back("Cache-Control", "public, max-age=86400");
            res.body.assign(a.data.data(), a.data.size());
        });
    }
}

} // namespace httpp
