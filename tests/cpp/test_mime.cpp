// The one MIME table (ADR-0047): used by mounted directories, and by Python file responses through httpp::mime_type_for.
#include "utest/utest.h"
#include "httpp/server.hpp"

#include <string>

UTEST(httpp_mime, known_extensions_are_mapped_and_case_does_not_matter) {
    ASSERT_STREQ("text/html", httpp::mime_type_for("/a/index.html"));
    ASSERT_STREQ("text/html", httpp::mime_type_for("INDEX.HTM"));
    ASSERT_STREQ("image/png", httpp::mime_type_for("x.PnG"));
    ASSERT_STREQ("application/wasm", httpp::mime_type_for("m.wasm"));
}

UTEST(httpp_mime, the_types_that_used_to_differ_between_the_server_and_python_are_fixed) {
    ASSERT_STREQ("image/x-icon", httpp::mime_type_for("favicon.ico"));
    ASSERT_STREQ("application/gzip", httpp::mime_type_for("data.gz"));
    ASSERT_STREQ("audio/wav", httpp::mime_type_for("a.wav"));
    ASSERT_STREQ("application/yaml", httpp::mime_type_for("conf.yaml"));
    ASSERT_STREQ("application/yaml", httpp::mime_type_for("conf.yml"));
}

UTEST(httpp_mime, an_unknown_or_missing_extension_is_octet_stream_and_a_dot_in_a_directory_is_not_an_extension) {
    ASSERT_STREQ("application/octet-stream", httpp::mime_type_for("file.unknownext"));
    ASSERT_STREQ("application/octet-stream", httpp::mime_type_for("Makefile"));
    ASSERT_STREQ("application/octet-stream", httpp::mime_type_for("/dir.html/file"));
    ASSERT_STREQ("application/octet-stream", httpp::mime_type_for("trailingdot."));
}
