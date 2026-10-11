#include "internal/log.hpp"

#include "aixlog.hpp" // vendored AixLog v1.5.0: only this file sees it

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>

namespace httpp {
namespace {

AixLog::Severity to_aix(log_level l) {
    switch (l) {
        case log_level::trace: return AixLog::Severity::trace;
        case log_level::debug: return AixLog::Severity::debug;
        case log_level::info: return AixLog::Severity::info;
        case log_level::warning: return AixLog::Severity::warning;
        case log_level::error: return AixLog::Severity::error;
        case log_level::off: break;
    }
    return AixLog::Severity::fatal;
}

log_level from_aix(AixLog::Severity s) {
    switch (s) {
        case AixLog::Severity::trace: return log_level::trace;
        case AixLog::Severity::debug: return log_level::debug;
        case AixLog::Severity::info:
        case AixLog::Severity::notice: return log_level::info;
        case AixLog::Severity::warning: return log_level::warning;
        default: return log_level::error;
    }
}

struct state {
    std::mutex mu;                                 // one log line at a time, and no logging while sinks are swapped
    std::atomic<int> level{static_cast<int>(log_level::warning)};
    std::unique_ptr<std::ostream> out;             // writes into AixLog's streambuf
    bool ready = false;

    // Vendored AixLog is patched not to redirect std::clog (see third_party/README.md), so we talk to
    // its streambuf through a private ostream and the process-wide clog is never touched.
    void ensure_ready() {
        if (ready) return;
        out = std::make_unique<std::ostream>(&AixLog::Log::instance());
        AixLog::Log::init({std::make_shared<AixLog::SinkCerr>(AixLog::Filter(AixLog::Severity::trace))});
        ready = true;
    }
};

state& st() {
    static state s; // never destroyed before the last log call from a late thread: leaked on purpose
    return s;
}

} // namespace

void set_log_level(log_level level) { st().level = static_cast<int>(level); }

bool set_log_file(const std::string& path) {
    auto& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.ensure_ready();
    if (path.empty()) {
        AixLog::Log::init({std::make_shared<AixLog::SinkCerr>(AixLog::Filter(AixLog::Severity::trace))});
        return true;
    }
    auto sink = std::make_shared<AixLog::SinkFile>(AixLog::Filter(AixLog::Severity::trace), path);
    if (!std::ifstream(path)) return false; // SinkFile opens in its constructor; check that it worked
    AixLog::Log::init({sink});
    return true;
}

void set_log_callback(log_callback fn) {
    auto& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.ensure_ready();
    if (!fn) {
        AixLog::Log::init({std::make_shared<AixLog::SinkCerr>(AixLog::Filter(AixLog::Severity::trace))});
        return;
    }
    AixLog::Log::init({std::make_shared<AixLog::SinkCallback>(
        AixLog::Filter(AixLog::Severity::trace),
        [fn = std::move(fn)](const AixLog::Metadata& m, const std::string& message) {
            fn(from_aix(m.severity), m.function ? m.function.name : std::string(), message);
        })});
}

namespace detail {

bool log_enabled(log_level level) {
    return static_cast<int>(level) >= st().level.load(std::memory_order_relaxed) && level != log_level::off;
}

void log_write(log_level level, const char* where, const std::string& message) {
    if (!log_enabled(level)) return;
    auto& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    s.ensure_ready();
    *s.out << to_aix(level) << AixLog::Tag("httpp") << AixLog::Timestamp(std::chrono::system_clock::now())
           << AixLog::Function(where, "", 0) << message << std::endl;
}

} // namespace detail
} // namespace httpp
