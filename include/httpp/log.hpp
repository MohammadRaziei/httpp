#pragma once

// Logging for the library itself (not for your application). Built on AixLog, which stays
// private to the library: it does not touch std::clog or your own logging setup.
//
// By default warnings and errors go to stderr. Quiet it with set_log_level(log_level::off), or send
// the lines to your own logger with set_log_callback().

#include "httpp/export.hpp"

#include <functional>
#include <string>

namespace httpp {

enum class log_level { trace, debug, info, warning, error, off };

// Messages below this level are dropped. Thread-safe.
HTTPP_API void set_log_level(log_level level);

// Writes the log to this file (truncated) instead of stderr; "" goes back to stderr.
// Returns false (and keeps the previous output) if the file cannot be opened.
HTTPP_API bool set_log_file(const std::string& path);

// Sends every message that passes the level to `fn(level, where, message)`, replacing stderr/file
// output. `where` is the function that logged. `fn` may be called from any library thread, one call
// at a time, and must not log through httpp itself. An empty function restores stderr.
using log_callback = std::function<void(log_level level, const std::string& where, const std::string& message)>;
HTTPP_API void set_log_callback(log_callback fn);

} // namespace httpp
