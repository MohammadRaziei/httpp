#pragma once

#include "httpp/export.hpp"

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

// The asset called `name` ("swagger-ui-bundle.js", "swagger-ui.css", "favicon-32x32.png"), or nullptr.
HTTPP_API const swagger_ui_asset* find_swagger_ui_asset(std::string_view name) noexcept;

} // namespace httpp
