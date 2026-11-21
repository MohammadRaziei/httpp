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
// file; `raw_size` is the size of the file itself either way. swagger::mount() sends the stored bytes as they are to
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

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251) // the std members of the exported classes are private and never touched by a caller
#endif

// Swagger UI served from the bytes compiled into the library (nothing is read from disk, ADR-0066):
// `swagger("/openapi.json").title("My API").mount(server, "/docs")` (ADR-0007).
// `openapi_url` is where the page loads the OpenAPI document from. It is required, because this class does not make or serve that
// document: you do, on that URL (any valid OpenAPI JSON). It is used as it is, so when the server is not mounted at the root, give the
// whole URL. mount() puts the page at `base` (with or without a trailing '/') and the three files under <base>/<name>; base defaults
// to "/docs" and must start with '/' and name more than the root, or mount() throws std::invalid_argument.
class HTTPP_API swagger : public mountable {
public:
    // Throws std::invalid_argument when `openapi_url` is empty.
    HTTPP_MEMBER explicit swagger(std::string openapi_url);
    HTTPP_MEMBER swagger& title(std::string title);            // shown in the tab (escaped); default "API"
    // `s` is a handle (ADR-0081): the page goes on the server it points to. Returns that handle. May be called more than once.
    HTTPP_MEMBER server mount(server s, const std::string& base = "/docs") const override;

private:
    std::string openapi_url_;
    std::string title_;
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace httpp
