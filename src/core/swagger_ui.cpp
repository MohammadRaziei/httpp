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
};

// The tab icon (the green {…} circle) lives in the page itself: no file, no route, no request to another site.
// Percent-encoded by hand (< > #), single quotes inside so it can sit in a double-quoted attribute.
constexpr const char* icon_data_uri =
    "data:image/svg+xml,%3Csvg viewBox='0 0 512 512' xmlns='http://www.w3.org/2000/svg' fill-rule='evenodd' clip-ru"
    "le='evenodd' stroke-linejoin='round' stroke-miterlimit='2'%3E%3Cpath d='M255.997 494.079c-131.277 0-238.084-10"
    "6.796-238.084-238.08 0-131.282 106.807-238.078 238.084-238.078 131.278 0 238.084 106.806 238.084 238.084 0 131"
    ".278-106.806 238.074-238.084 238.074z' fill='%2385ea2d' fill-rule='nonzero'/%3E%3Cpath d='M255.997 29.834C380."
    "91 29.834 482.163 131.09 482.163 256c0 124.906-101.254 226.165-226.166 226.165-124.906 0-226.165-101.259-226.1"
    "65-226.165 0-124.906 101.26-226.166 226.165-226.166m0-23.831C118.148 6.003 6 118.15 6 256c0 137.85 112.148 249"
    ".997 249.997 249.997C393.847 505.997 506 393.85 506 256 506 118.15 393.847 6.002 255.997 6.002z' fill='%231736"
    "47' fill-rule='nonzero'/%3E%3Cpath d='M163.418 175.18c-.77 8.555.285 17.4-.284 26.05-.675 8.655-1.735 17.21-3."
    "464 25.766-2.403 12.208-10 21.438-20.473 29.129 20.383 13.268 22.687 33.836 24.031 54.7.675 11.249.385 22.592 "
    "1.54 33.741.864 8.65 4.227 10.864 13.172 11.15 3.653.094 7.396 0 11.634 0v26.724c-26.435 4.518-48.258-2.978-53"
    ".645-25.376-1.735-8.17-2.889-16.536-3.269-24.996-.58-8.94.38-17.88-.29-26.82-1.923-24.517-5.097-32.782-28.549-"
    "33.937v-30.473a46.316 46.316 0 015.092-.864c12.883-.675 18.36-4.618 21.149-17.306 1.349-7.11 2.119-14.322 2.40"
    "3-21.628.965-13.937.58-28.164 2.979-42.007 3.363-19.899 15.671-29.513 36.15-30.668 5.767-.29 11.633 0 18.265 0"
    "v27.305c-2.789.195-5.192.575-7.69.575-16.632-.58-17.501 5.092-18.75 18.934zm32.013 63.255h-.385c-9.615-.48-17."
    "88 7.016-18.365 16.63-.48 9.71 7.016 17.98 16.63 18.455h1.155c9.52.58 17.686-6.731 18.265-16.246v-.96c.195-9.7"
    "09-7.59-17.685-17.3-17.88zm60.277 0c-9.325-.29-17.116 7.016-17.4 16.246 0 .58 0 1.06.1 1.634 0 10.479 7.11 17."
    "21 17.88 17.21 10.573 0 17.21-6.92 17.21-17.785-.1-10.48-7.121-17.4-17.79-17.305zm61.72 0c-9.804-.195-17.98 7."
    "595-18.264 17.4 0 9.805 7.88 17.685 17.685 17.685h.195c8.845 1.54 17.785-7.016 18.365-17.3.474-9.52-8.176-17.7"
    "85-17.98-17.785zm84.694 1.439c-11.154-.48-16.73-4.228-19.514-14.807-1.734-6.726-2.789-13.747-3.173-20.669-.77-"
    "12.883-.675-25.86-1.54-38.743-2.019-30.569-24.126-41.238-56.239-35.956v26.536c5.092 0 9.035 0 12.978.095 6.826"
    ".095 12.019 2.693 12.688 10.29.675 6.92.675 13.936 1.35 20.958 1.349 13.942 2.113 28.07 4.517 41.817 2.119 11."
    "343 9.904 19.804 19.61 26.725-17.011 11.439-22.014 27.78-22.878 46.144-.48 12.594-.77 25.282-1.44 37.975-.58 1"
    "1.533-4.612 15.281-16.246 15.571-3.268.095-6.441.385-10.094.58v27.205c6.826 0 13.073.384 19.32 0 19.419-1.155 "
    "31.147-10.574 34.995-29.42a245.277 245.277 0 002.884-31.337c.67-9.615.58-19.324 1.539-28.839 1.439-14.902 8.26"
    "5-21.053 23.167-22.013 1.444-.195 2.788-.48 4.138-.96v-30.478c-2.504-.29-4.238-.574-6.062-.674z' fill='%231736"
    "47' fill-rule='nonzero'/%3E%3C/svg%3E";

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
        " - Swagger UI</title>\n<link rel=\"icon\" type=\"image/svg+xml\" href=\"" + std::string(icon_data_uri) + "\">\n"
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
