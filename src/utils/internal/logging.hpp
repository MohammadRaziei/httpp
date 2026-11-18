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
// is the logger's level (ADR-0091). The registry sets it up when it is created, so this is the public get_logger() and nothing more; the
// reference is looked up once per module (the lookup takes a lock), the check that follows is the inline logger::enabled().
inline logging::logger& lib_logger() {
    static logging::logger& lg = logging::get_logger("httpp");
    return lg;
}

} // namespace httpp::detail

// The library's own log is the public macro on the library's logger: the same laziness, the same stream buffer, no second system.
#define HTTPP_LIB_LOG(level_name) HTTPP_LOG(::httpp::detail::lib_logger(), level_name)
