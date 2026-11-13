#pragma once

#include "httpp/export.hpp"
#include "httpp/server.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace httpp {

// One file of Swagger UI (https://swagger.io/tools/swagger-ui/, Apache-2.0). The files are compiled into libhttpp
// at CMake configure time (cmake/EmbedFile.cmake, sources in src/third_party/swagger-ui): nothing is read from
// disk at run time, so /docs works offline from any install.
//
// The bytes are stored gzip-compressed (about a quarter of the size, ADR-0031) unless libhttpp was built with
// -DHTTPP_EMBED_COMPRESS=OFF. `data` is what is stored: when `gzip` is true it is a gzip stream (RFC 1952), not the
// file; `raw_size` is the size of the file itself either way. swagger_ui::mount() sends the stored bytes as they are to
// a client that accepts gzip, and inflates them for one that does not.
struct swagger_ui_asset {
    std::string_view name;          // "swagger-ui-bundle.js"
    std::string_view content_type;  // "text/javascript"
    std::string_view data;          // the stored bytes: a gzip stream when `gzip` is true, else the file; valid for the life of the process
    std::size_t raw_size;           // the size of the file, whatever is stored
    bool gzip;                      // `data` is a gzip stream
};

// The asset called `name` ("swagger-ui-bundle.js", "swagger-ui.css", "swagger-api-icon.svg"), or nullptr.
HTTPP_API const swagger_ui_asset* find_swagger_ui_asset(std::string_view name) noexcept;

// Swagger UI served from the bytes compiled into the library (nothing is read from disk, ADR-0066):
// `swagger_ui().set_docs_path("/docs").set_openapi_url("/openapi.json").set_title("My API").mount(server)`.
// The page is at the docs path (with or without a trailing '/') and the three files under <docs path>/<name>. The OpenAPI document is
// served by you. set_docs_path throws std::invalid_argument unless the path starts with '/' and names more than the root (a trailing '/'
// is accepted: "/docs/" and "/docs" give the same file URLs); a refused value leaves the object unchanged.
class swagger_ui {
public:
    HTTPP_API swagger_ui();
    HTTPP_API swagger_ui& set_docs_path(std::string path);      // default "/docs"
    HTTPP_API swagger_ui& set_openapi_url(std::string url);     // where the page loads the OpenAPI document from; default "/openapi.json"
    HTTPP_API swagger_ui& set_title(std::string title);         // shown in the tab (escaped); default "API"
    HTTPP_API void mount(server& s) const;

private:
    std::string docs_path_;
    std::string openapi_url_;
    std::string title_;
};

} // namespace httpp
