#pragma once

// Internal. The macro used inside the library to log: the public HTTPP_LOG (httpp/logging.hpp, ADR-0090) on the library's own logger
// "httpp" (ADR-0091). There is no second logging system behind it.
//
//   HTTPP_LIB_LOG(warning) << "handler threw: " << what;
//
// A disabled level costs two relaxed loads and a compare (logger::enabled, ADR-0090); the message is not even built.

#include "httpp/export.hpp"
#include "httpp/logging.hpp"


namespace httpp::detail {

// The library's own logger, "httpp": the one httpp::set_log_level / set_log_file / set_log_callback / set_log_system configure, so its level
// is the logger's level (ADR-0091). Exported (unlike the rest of src/core/internal) so the engine tests, which compile engine sources
// straight into the test binary, share the library's logger instead of having a second one.
HTTPP_UTILS_API logging::logger& lib_logger();

// The reference is looked up once per module; the call that follows is the public, inline logger::enabled().
inline logging::logger& lib_log() {
    static logging::logger& lg = lib_logger();
    return lg;
}

} // namespace httpp::detail

// The library's own log is the public macro on the library's logger: the same laziness, the same stream buffer, no second system.
#define HTTPP_LIB_LOG(level_name) HTTPP_LOG(::httpp::detail::lib_log(), level_name)
