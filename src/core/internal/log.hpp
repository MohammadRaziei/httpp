#pragma once

// Internal. Library logging without pulling AixLog's global macros (LOG, TAG, ERROR, DEBUG ...)
// into every file: only src/core/log.cpp includes AixLog.
//
//   HTTPP_LOG(warning) << "handler threw: " << what;
//
// A disabled level costs one atomic load; the message is not even built.

#include "httpp/export.hpp"
#include "httpp/log.hpp"

#include <sstream>
#include <string>

namespace httpp::detail {

// Exported (unlike the rest of src/core/internal) so the engine tests, which compile engine sources
// straight into the test binary, share the library's log state instead of having a second one.
HTTPP_API bool log_enabled(log_level level);
HTTPP_API void log_write(log_level level, const char* where, const std::string& message);

class log_line {
public:
    log_line(log_level level, const char* where) : level_(level), where_(where) {}
    ~log_line() { log_write(level_, where_, os_.str()); }
    log_line(const log_line&) = delete;
    log_line& operator=(const log_line&) = delete;
    template <class T>
    log_line& operator<<(const T& value) {
        os_ << value;
        return *this;
    }

private:
    log_level level_;
    const char* where_;
    std::ostringstream os_;
};

} // namespace httpp::detail

#define HTTPP_LOG(level_name) \
    if (!::httpp::detail::log_enabled(::httpp::log_level::level_name)) {} \
    else ::httpp::detail::log_line(::httpp::log_level::level_name, __func__)
