#include "httpp/swagger_ui.hpp"

#include "httpp_swagger_ui_data.hpp" // generated at configure time, see CMakeLists.txt
#include "internal/inflate.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <stdexcept>
#include <string>

namespace httpp {

namespace {

std::string_view bytes(const unsigned char* data, std::size_t size) noexcept {
    return {reinterpret_cast<const char*>(data), size};
}

const swagger_ui_asset assets[] = {
    {"swagger-ui-bundle.js", "text/javascript", bytes(embedded::swagger_ui_bundle_js, embedded::swagger_ui_bundle_js_size),
     embedded::swagger_ui_bundle_js_raw_size, embedded::swagger_ui_bundle_js_is_gzip},
    {"swagger-ui.css", "text/css", bytes(embedded::swagger_ui_css, embedded::swagger_ui_css_size),
     embedded::swagger_ui_css_raw_size, embedded::swagger_ui_css_is_gzip},
    {"swagger-api-icon.svg", "image/svg+xml", bytes(embedded::swagger_ui_icon_svg, embedded::swagger_ui_icon_svg_size),
     embedded::swagger_ui_icon_svg_raw_size, embedded::swagger_ui_icon_svg_is_gzip},
};

// True when an Accept-Encoding value lists gzip (or x-gzip) with a quality above zero.
// ponytail: "*" is not read as gzip (the original bytes are always a safe answer), and a quality is only tested for zero.
bool accepts_gzip(const std::string& header) {
    std::size_t pos = 0;
    while (pos <= header.size()) {
        std::size_t end = header.find(',', pos);
        if (end == std::string::npos) end = header.size();
        const std::string item = header.substr(pos, end - pos);
        pos = end + 1;
        const std::size_t semi = item.find(';');
        std::string name = item.substr(0, semi);
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name != "gzip" && name != "x-gzip") continue;
        bool zero = false; // "q=0", "q=0.0", "q=0.000"
        if (semi != std::string::npos) {
            std::string params = item.substr(semi + 1);
            params.erase(std::remove(params.begin(), params.end(), ' '), params.end());
            const std::size_t q = params.find("q=");
            if (q != std::string::npos) {
                const std::size_t stop = params.find(';', q);
                const std::string value = params.substr(q + 2, stop == std::string::npos ? std::string::npos : stop - q - 2);
                zero = !value.empty() && value.find_first_not_of("0.") == std::string::npos;
            }
        }
        if (!zero) return true;
    }
    return false;
}

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

swagger_ui::swagger_ui() : docs_path_("/docs"), openapi_url_("/openapi.json"), title_("API") {}

swagger_ui& swagger_ui::set_docs_path(std::string path) {
    std::string base = path;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (path.empty() || path[0] != '/' || base.empty())
        throw std::invalid_argument("swagger_ui: docs_path must start with '/' and be more than the root");
    docs_path_ = std::move(path);
    return *this;
}

swagger_ui& swagger_ui::set_openapi_url(std::string url) { openapi_url_ = std::move(url); return *this; }
swagger_ui& swagger_ui::set_title(std::string title) { title_ = std::move(title); return *this; }

void swagger_ui::mount(server& s) const {
    const std::string& openapi_url = openapi_url_;
    const std::string& title = title_;
    std::string base = docs_path_;
    while (!base.empty() && base.back() == '/') base.pop_back();

    const std::string safe_base = html_escape(base);
    const std::string page =
        "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>" + html_escape(title) +
        " - Swagger UI</title>\n<link rel=\"icon\" type=\"image/svg+xml\" href=\"" + safe_base + "/swagger-api-icon.svg\">\n"
        "<link rel=\"stylesheet\" href=\"" + safe_base + "/swagger-ui.css\">\n</head>\n<body>\n<div id=\"swagger-ui\"></div>\n"
        "<script src=\"" + safe_base + "/swagger-ui-bundle.js\"></script>\n<script>\n"
        "window.ui = SwaggerUIBundle({url: " + js_string(openapi_url) + ", dom_id: \"#swagger-ui\", layout: \"BaseLayout\", "
        "deepLinking: true,\n  presets: [SwaggerUIBundle.presets.apis]});\n</script>\n</body>\n</html>\n";

    // The page answers both "/docs" and "/docs/": people type either.
    for (const std::string& path : {base, base + "/"}) {
        s.get(path, [page](const server_request&, response& res) {
            res.status = 200;
            res.headers.emplace_back("Content-Type", "text/html; charset=utf-8");
            res.body = page;
        });
    }
    for (const auto& a : assets) {
        // ponytail: res.body is a std::string, so each request copies the stored bytes (about 430 KB of gzip for the
        // bundle, 1.5 MB when stored as they are). Fine for a page opened by hand; the upgrade is a string_view body in
        // response, which the engine would send as is. A client that does not take gzip gets the file inflated on
        // every request, nothing is cached: that is old tools (curl without --compressed), not a browser.
        s.get(base + "/" + std::string(a.name), [&a](const server_request& req, response& res) {
            res.status = 200;
            res.headers.emplace_back("Content-Type", std::string(a.content_type));
            res.headers.emplace_back("Cache-Control", "public, max-age=86400");
            if (!a.gzip) {
                res.body.assign(a.data.data(), a.data.size());
                return;
            }
            res.headers.emplace_back("Vary", "Accept-Encoding"); // two versions of one URL
            if (accepts_gzip(req.header("Accept-Encoding"))) {
                res.headers.emplace_back("Content-Encoding", "gzip"); // set_compression() leaves a body with this header alone
                res.body.assign(a.data.data(), a.data.size());
            } else {
                res.body = detail::inflate(a.data.data(), a.data.size(), detail::inflate_format::gzip, a.raw_size + 1);
            }
        });
    }
}

} // namespace httpp
