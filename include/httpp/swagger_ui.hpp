#pragma once

#include "httpp/export.hpp"
#include "httpp/server.hpp"

#include <string>
#include <string_view>

namespace httpp {

// One file of Swagger UI (https://swagger.io/tools/swagger-ui/, Apache-2.0). The files are compiled into libhttpp
// at CMake configure time (cmake/EmbedFile.cmake, sources in src/third_party/swagger-ui): nothing is read from
// disk at run time, so /docs works offline from any install.
struct swagger_ui_asset {
    std::string_view name;          // "swagger-ui-bundle.js"
    std::string_view content_type;  // "text/javascript"
    std::string_view data;          // the file's bytes; valid for the life of the process
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
