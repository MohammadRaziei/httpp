#include "internal/logging_os.hpp"

#include <climits>
#include <string>
#include <utility>

#include <stdexcept>

#if HTTPP_LOG_HAS_SYSLOG
#include <syslog.h>
#endif

#if HTTPP_LOG_HAS_ANDROID
#include <android/log.h>
#endif

#if HTTPP_LOG_HAS_UNIFIED
#include <os/log.h>

#include <string_view>
#include <unordered_map>
#endif

#if HTTPP_LOG_HAS_DEBUGSTRING || HTTPP_LOG_HAS_EVENTLOG
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace httpp::logging {

// ------------------------------------------------------------------------------------------ syslog

#if HTTPP_LOG_HAS_SYSLOG

struct syslog_handler::state {
    std::string ident;  // openlog() keeps this pointer, so the string stays where it is: the state is on the heap
    ~state() { closelog(); }
};

namespace {

int syslog_facility_code(syslog_facility f) noexcept {
    switch (f) {
        case syslog_facility::user: return LOG_USER;
        case syslog_facility::daemon: return LOG_DAEMON;
        case syslog_facility::local0: return LOG_LOCAL0;
        case syslog_facility::local1: return LOG_LOCAL1;
        case syslog_facility::local2: return LOG_LOCAL2;
        case syslog_facility::local3: return LOG_LOCAL3;
        case syslog_facility::local4: return LOG_LOCAL4;
        case syslog_facility::local5: return LOG_LOCAL5;
        case syslog_facility::local6: return LOG_LOCAL6;
        case syslog_facility::local7: return LOG_LOCAL7;
    }
    return LOG_USER;
}

int syslog_priority(level lv) noexcept {
    switch (detail::severity_rank(lv)) {
        case detail::rank_critical: return LOG_CRIT;
        case detail::rank_error: return LOG_ERR;
        case detail::rank_warning: return LOG_WARNING;
        case detail::rank_info: return LOG_INFO;
        default: return LOG_DEBUG;
    }
}

}  // namespace

syslog_handler::syslog_handler(std::string ident, syslog_facility facility, bool log_pid)
    : st_(std::make_unique<state>()) {
    st_->ident = std::move(ident);
    openlog(st_->ident.c_str(), log_pid ? LOG_PID : 0, syslog_facility_code(facility));
    set_formatter(formatter("%(name)s: %(message)s"));
}

syslog_handler::~syslog_handler() = default;
syslog_handler::syslog_handler(syslog_handler&&) noexcept = default;
syslog_handler& syslog_handler::operator=(syslog_handler&&) noexcept = default;

void syslog_handler::emit(const record& r) {
    const std::string line = format(r);
    syslog(syslog_priority(r.levelno), "%s", line.c_str());  // never the text as the format
}

#endif  // HTTPP_LOG_HAS_SYSLOG

// ----------------------------------------------------------------------------------------- Android

#if HTTPP_LOG_HAS_ANDROID

namespace {

int android_priority(level lv) noexcept {
    switch (detail::severity_rank(lv)) {
        case detail::rank_critical: return ANDROID_LOG_FATAL;
        case detail::rank_error: return ANDROID_LOG_ERROR;
        case detail::rank_warning: return ANDROID_LOG_WARN;
        case detail::rank_info: return ANDROID_LOG_INFO;
        default: return ANDROID_LOG_DEBUG;
    }
}

}  // namespace

android_handler::android_handler(std::string tag) : tag_(std::move(tag)) {
    set_formatter(formatter("%(message)s"));
}

void android_handler::emit(const record& r) {
    const std::string line = format(r);
    const std::string tag = tag_.empty() ? std::string(r.name) : tag_;
    __android_log_write(android_priority(r.levelno), tag.c_str(), line.c_str());
}

#endif  // HTTPP_LOG_HAS_ANDROID

// --------------------------------------------------------------------------- Apple unified logging

#if HTTPP_LOG_HAS_UNIFIED

struct unified_log_handler::state {
    std::string subsystem;
    std::unordered_map<std::string, os_log_t> logs;  // one os_log object per category (= logger name), made on first use

    os_log_t log_for(std::string_view category) {
        const std::string key(category);
        const auto it = logs.find(key);
        if (it != logs.end()) return it->second;
        os_log_t made = os_log_create(subsystem.c_str(), key.c_str());
        if (made == nullptr) return OS_LOG_DEFAULT;  // not kept: nothing to release
        logs.emplace(key, made);
        return made;
    }

    ~state() {
        for (const auto& entry : logs) os_release(entry.second);
    }
};

namespace {

os_log_type_t unified_type(level lv) noexcept {
    switch (detail::severity_rank(lv)) {
        case detail::rank_critical: return OS_LOG_TYPE_FAULT;
        case detail::rank_error: return OS_LOG_TYPE_ERROR;
        case detail::rank_warning: return OS_LOG_TYPE_DEFAULT;
        case detail::rank_info: return OS_LOG_TYPE_INFO;
        default: return OS_LOG_TYPE_DEBUG;
    }
}

}  // namespace

unified_log_handler::unified_log_handler(std::string subsystem) : st_(std::make_unique<state>()) {
    st_->subsystem = std::move(subsystem);
    set_formatter(formatter("%(message)s"));
}

unified_log_handler::~unified_log_handler() = default;
unified_log_handler::unified_log_handler(unified_log_handler&&) noexcept = default;
unified_log_handler& unified_log_handler::operator=(unified_log_handler&&) noexcept = default;

void unified_log_handler::emit(const record& r) {
    const std::string line = format(r);
    const os_log_t log = st_->log_for(r.name);  // emit() runs under the handler's lock: the map needs none
    os_log_with_type(log, unified_type(r.levelno), "%{public}s", line.c_str());
}

#endif  // HTTPP_LOG_HAS_UNIFIED

// ------------------------------------------------------------------------------------------ Windows

#if HTTPP_LOG_HAS_DEBUGSTRING || HTTPP_LOG_HAS_EVENTLOG

namespace {

// UTF-8 to UTF-16, which is what the wide Windows calls take. Invalid bytes become U+FFFD.
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int len = static_cast<int>(s.size() > static_cast<std::size_t>(INT_MAX) ? static_cast<std::size_t>(INT_MAX) : s.size());
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), len, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), len, w.data(), n);
    return w;
}

}  // namespace

#endif

#if HTTPP_LOG_HAS_DEBUGSTRING

debug_string_handler::debug_string_handler() { set_formatter(formatter("[%(levelname)s] %(name)s: %(message)s")); }

void debug_string_handler::emit(const record& r) {
    const std::wstring line = widen(format(r) + "\n");
    OutputDebugStringW(line.c_str());
}

#endif  // HTTPP_LOG_HAS_DEBUGSTRING

#if HTTPP_LOG_HAS_EVENTLOG

struct event_log_handler::state {
    HANDLE source = nullptr;
    ~state() {
        if (source != nullptr) DeregisterEventSource(source);
    }
};

event_log_handler::event_log_handler(std::string source) : st_(std::make_unique<state>()) {
    const std::wstring name = widen(source);
    st_->source = RegisterEventSourceW(nullptr, name.c_str());
    if (st_->source == nullptr)
        throw std::runtime_error("cannot register the event source " + source);
    set_formatter(formatter("%(name)s: %(message)s"));
}

event_log_handler::~event_log_handler() = default;
event_log_handler::event_log_handler(event_log_handler&&) noexcept = default;
event_log_handler& event_log_handler::operator=(event_log_handler&&) noexcept = default;

void event_log_handler::emit(const record& r) {
    constexpr std::size_t max_chars = 31000;  // the Event Log refuses a string of 31,839 characters or more
    std::wstring text = widen(format(r));
    if (text.size() > max_chars) text.resize(max_chars);
    const int severity = detail::severity_rank(r.levelno);
    const WORD type = static_cast<WORD>(severity <= detail::rank_info      ? EVENTLOG_INFORMATION_TYPE
                                        : severity == detail::rank_warning ? EVENTLOG_WARNING_TYPE
                                                                           : EVENTLOG_ERROR_TYPE);
    LPCWSTR strings[1] = {text.c_str()};
    ReportEventW(st_->source, type, 0, static_cast<DWORD>(1 + severity), nullptr, 1, 0, strings, nullptr);
}

#endif  // HTTPP_LOG_HAS_EVENTLOG

}  // namespace httpp::logging
