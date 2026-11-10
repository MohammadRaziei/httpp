#include "internal/log.hpp"

#include "internal/logging_os.hpp"
#include "httpp/logging.hpp"

#include <atomic>
#include <mutex>
#include <utility>

namespace httpp {
namespace {

logging::level to_level(log_level l) {
    switch (l) {
        case log_level::trace: return logging::level::trace;
        case log_level::debug: return logging::level::debug;
        case log_level::info: return logging::level::info;
        case log_level::warning: return logging::level::warning;
        case log_level::error: return logging::level::error;
        case log_level::off: break;
    }
    return logging::level::critical;
}

log_level from_level(logging::level l) {
    if (int(l) <= int(logging::level::trace)) return log_level::trace;
    if (int(l) <= int(logging::level::debug)) return log_level::debug;
    if (int(l) <= int(logging::level::info)) return log_level::info;
    if (int(l) <= int(logging::level::warning)) return log_level::warning;
    return log_level::error;
}

constexpr const char* k_pattern = "%(asctime)s [%(levelname)s] %(name)s (%(funcName)s): %(message)s";

struct state {
    std::mutex mu;                                 // swapping the output, so a line never meets a half-changed setup
    std::atomic<int> level{static_cast<int>(log_level::warning)};
    logging::logger* lg;

    state() : lg(&logging::get_logger("httpp")) {
        lg->set_level(logging::level::trace);      // the filtering is done by `level` above (one atomic load, no message built)
        lg->set_propagate(false);                  // the application's own logging setup is never touched
        use_stderr();
    }
    template <class H>
    void use(H h, const char* pattern = k_pattern) {
        h.set_formatter(logging::formatter(pattern));
        lg->clear_handlers();
        lg->add_handler(std::move(h));
    }
    void use_stderr() { use(logging::stream_handler(stderr)); }
};

state& st() {
    static state* s = new state; // leaked on purpose: a late thread may still log while the process exits
    return *s;
}

} // namespace

void set_log_level(log_level level) { st().level = static_cast<int>(level); }

bool set_log_file(const path& file) {
    const std::string& path = file.str();
    auto& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (path.empty()) {
        s.use_stderr();
        return true;
    }
    try {
        s.use(logging::file_handler(path, false)); // truncates; throws when the file cannot be opened
    } catch (const std::exception&) {
        return false; // the previous output is kept
    }
    return true;
}

bool set_log_system(log_system which, const std::string& name) {
    auto& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    constexpr const char* plain = "%(funcName)s: %(message)s"; // the system adds time, level and process itself
    if (which == log_system::native) {
#if HTTPP_LOG_HAS_ANDROID
        which = log_system::android;
#elif HTTPP_LOG_HAS_UNIFIED
        which = log_system::unified;
#elif HTTPP_LOG_HAS_DEBUGSTRING
        which = log_system::debug_string;
#else
        which = log_system::syslog;
#endif
    }
    try {
        switch (which) {
#if HTTPP_LOG_HAS_SYSLOG
            case log_system::syslog:
                s.lg->clear_handlers(); // closelog() of the old handler must not come after our openlog()
                s.use(logging::syslog_handler(name.empty() ? "httpp" : name), plain);
                return true;
#endif
#if HTTPP_LOG_HAS_ANDROID
            case log_system::android:
                s.use(logging::android_handler(name == "httpp" ? std::string() : name), plain);
                return true;
#endif
#if HTTPP_LOG_HAS_UNIFIED
            case log_system::unified:
                s.use(logging::unified_log_handler(name.empty() ? "httpp" : name), plain);
                return true;
#endif
#if HTTPP_LOG_HAS_DEBUGSTRING
            case log_system::debug_string:
                s.use(logging::debug_string_handler(), "[%(levelname)s] %(funcName)s: %(message)s");
                return true;
            case log_system::event_log:
                s.use(logging::event_log_handler(name.empty() ? "httpp" : name), plain);
                return true;
#endif
            default: break;
        }
    } catch (const std::exception&) {
    }
    return false; // not on this platform, or it could not be opened: the previous output is kept
}

void set_log_callback(log_callback fn) {
    auto& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!fn) {
        s.use_stderr();
        return;
    }
    s.use(logging::callback_handler([fn = std::move(fn)](const logging::record& r) {
        fn(from_level(r.levelno), std::string(r.where), std::string(r.message));
    }));
}

namespace detail {

bool log_enabled(log_level level) {
    return static_cast<int>(level) >= st().level.load(std::memory_order_relaxed) && level != log_level::off;
}

void log_write(log_level level, const char* where, const std::string& message) {
    if (!log_enabled(level)) return;
    st().lg->log_from(to_level(level), where, message);
}

} // namespace detail
} // namespace httpp
