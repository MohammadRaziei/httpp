#pragma once
// The original bytes of a Swagger UI file compiled into the library: the stored bytes as they are, or the stored gzip stream inflated.
#include "httpp/swagger_ui.hpp"
#include "internal/inflate.hpp"

#include <string>

namespace testutil {

inline std::string swagger_original(const httpp::swagger_ui_asset& a) {
    if (!a.gzip) return std::string(a.data);
    // the limit is one byte over the recorded size: a stream that expands further than raw_size fails the test
    return httpp::detail::inflate(a.data.data(), a.data.size(), httpp::detail::inflate_format::gzip, a.raw_size + 1);
}

} // namespace testutil
