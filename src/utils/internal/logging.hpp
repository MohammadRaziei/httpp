#pragma once

// Internal. The macro used inside the library to log; src/core/logging.cpp turns it into a record of the
// logging module (httpp/logging.hpp, ADR-0050, 0057).
//
//   HTTPP_LIB_LOG(warning) << "handler threw: " << what;
//
// A disabled level costs one relaxed atomic load of a variable that is read inline (no call into libhttpp_utils); the message is not
// even built. The variable lives in libhttpp_utils (one per process) and is constant-initialized, so it is valid before any static
// initializer runs.

#include "httpp/export.hpp"
#include "httpp/logging.hpp"

#include <atomic>
#include <sstream>
#include <string>

namespace httpp::detail {

// Exported (unlike the rest of src/core/internal) so the engine tests, which compile engine sources
// straight into the test binary, share the library's log state instead of having a second one.
// The threshold of the library's own log (httpp::set_log_level writes it). Defined in logging.cpp, never inline here: a copy per
// module would be a second state.
HTTPP_UTILS_API extern std::atomic<int> own_log_level;
HTTPP_UTILS_API void log_write(logging::level level, const char* where, const std::string& message);

inline bool log_enabled(logging::level level) {
    return static_cast<int>(level) >= own_log_level.load(std::memory_order_relaxed) && level != logging::level::off;
}

class log_line {
public:
    log_line(logging::level level, const char* where) : level_(level), where_(where) {}
    ~log_line() { log_write(level_, where_, os_.str()); }
    log_line(const log_line&) = delete;
    log_line& operator=(const log_line&) = delete;
    template <class T>
    log_line& operator<<(const T& value) {
        os_ << value;
        return *this;
    }

private:
    logging::level level_;
    const char* where_;
    std::ostringstream os_;
};

} // namespace httpp::detail

#define HTTPP_LIB_LOG(level_name) \
    if (!::httpp::detail::log_enabled(::httpp::logging::level::level_name)) {} \
    else ::httpp::detail::log_line(::httpp::logging::level::level_name, __func__)
