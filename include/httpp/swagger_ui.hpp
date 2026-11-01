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
// file; `raw_size` is the size of the file itself either way. mount_swagger_ui() sends the stored bytes as they are to
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

// Serve Swagger UI from `s`: the page at `docs_path` (with or without a trailing '/') and the three files under `docs_path`/<name>, all from the
// bytes compiled into the library (nothing is read from disk). `openapi_url` is where the page loads the OpenAPI
// document from; you serve that yourself. `docs_path` must start with '/' and name more than the root, otherwise
// std::invalid_argument. A trailing '/' is accepted ("/docs/" and "/docs" give the same file URLs).
HTTPP_API void mount_swagger_ui(server& s, const std::string& docs_path = "/docs",
                                const std::string& openapi_url = "/openapi.json", const std::string& title = "API");

} // namespace httpp
