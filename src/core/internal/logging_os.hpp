#pragma once

// Internal (ADR-0056), ported from libstb: httpp::logging handlers that write to the log of the operating system, so a program's records
// show up where its platform's own tools look (journalctl, adb logcat, the Console app, Visual
// Studio's output window, Event Viewer):
//
//   syslog_handler         POSIX syslog(3): Linux, the BSDs, macOS            HTTPP_LOG_HAS_SYSLOG
//   android_handler        Android's logcat                                    HTTPP_LOG_HAS_ANDROID
//   unified_log_handler    Apple's unified logging (os_log): macOS, iOS        HTTPP_LOG_HAS_UNIFIED
//   debug_string_handler   Windows OutputDebugString (debugger, DebugView)     HTTPP_LOG_HAS_DEBUGSTRING
//   event_log_handler      Windows Event Log                                   HTTPP_LOG_HAS_EVENTLOG
//
// A class exists only on the platforms that have its facility, and the macro beside it says so (1 or
// 0), so portable code can ask:
//
//     #if HTTPP_LOG_HAS_SYSLOG
//     httpp::logging::get_logger("httpp").add_handler(httpp::logging::syslog_handler("myapp"));
//     #endif
//
// They are handlers like any other (see logging.hpp): they have a level, filters and a formatter,
// they can be moved but not copied, and a failing OS call is never an error to the caller. What
// differs is the default formatter. The OS adds its own time, level and process to every line, so
// the default pattern leaves them out (and says only what the OS cannot know: the logger's name, or
// nothing where the name is already the tag). set_formatter() changes that.
//
// The levels map to the five severities every one of these logs has: debug, info, warning, error,
// critical; a level in between (25) takes the severity below it.

#include <memory>
#include <string>
#include "httpp/logging.hpp"

#if defined(_WIN32)
#define HTTPP_LOG_HAS_DEBUGSTRING 1
#define HTTPP_LOG_HAS_EVENTLOG 1
#else
#define HTTPP_LOG_HAS_DEBUGSTRING 0
#define HTTPP_LOG_HAS_EVENTLOG 0
#endif

#if defined(__ANDROID__)
#define HTTPP_LOG_HAS_ANDROID 1
#else
#define HTTPP_LOG_HAS_ANDROID 0
#endif

#if defined(__APPLE__) && defined(__clang__)  // os_log needs the compiler's os_log builtins
#define HTTPP_LOG_HAS_UNIFIED 1
#else
#define HTTPP_LOG_HAS_UNIFIED 0
#endif

#if (defined(__unix__) || defined(__APPLE__)) && !defined(__ANDROID__)  // on Android, syslog is logcat: use android_handler
#define HTTPP_LOG_HAS_SYSLOG 1
#else
#define HTTPP_LOG_HAS_SYSLOG 0
#endif

namespace httpp::logging {

namespace detail {

// How the handlers sort a level into the five severities every operating system log has. A level between two named
// ones takes the one below it (25 is info), so each handler turns the rank into its own constants.
enum rank : int { rank_debug = 0, rank_info = 1, rank_warning = 2, rank_error = 3, rank_critical = 4 };

constexpr int severity_rank(level lv) noexcept {
    const int v = static_cast<int>(lv);
    return v >= static_cast<int>(level::critical) ? rank_critical
           : v >= static_cast<int>(level::error)  ? rank_error
           : v >= static_cast<int>(level::warning) ? rank_warning
           : v >= static_cast<int>(level::info)   ? rank_info
                                                  : rank_debug;
}

} // namespace detail

#if HTTPP_LOG_HAS_SYSLOG

// The facility a syslog message is filed under (what /etc/syslog.conf or journald sorts by).
enum class syslog_facility { user, daemon, local0, local1, local2, local3, local4, local5, local6, local7 };

// Sends each record to syslog(3) with the matching priority (LOG_DEBUG ... LOG_CRIT). The default
// pattern is "%(name)s: %(message)s": syslog adds the time, host, ident and (with log_pid) the pid.
//
// syslog has ONE connection per process: constructing a handler calls openlog(), so the most recent
// ident and facility win for every syslog user in the program, and destroying a handler calls
// closelog(). Use one syslog_handler per program, on the logger you want to send (usually "httpp").
class syslog_handler : public handler {
public:
    explicit syslog_handler(std::string ident = "httpp", syslog_facility facility = syslog_facility::user,
                            bool log_pid = true);
    ~syslog_handler() override;
    syslog_handler(syslog_handler&&) noexcept;
    syslog_handler& operator=(syslog_handler&&) noexcept;

protected:
    void emit(const record& r) override;

private:
    struct state;
    std::unique_ptr<state> st_;
};

#endif  // HTTPP_LOG_HAS_SYSLOG

#if HTTPP_LOG_HAS_ANDROID

// Sends each record to logcat (__android_log_write) with the matching priority (debug ... fatal for
// critical). `tag` is what `adb logcat <tag>:D *:S` filters on; empty (the default) uses the name of
// the record's logger, so "httpp.simd" can be switched on by itself. The default pattern is just
// "%(message)s": logcat adds the time, level, tag and pid. logcat cuts a message at about 4 KB.
class android_handler : public handler {
public:
    explicit android_handler(std::string tag = {});

protected:
    void emit(const record& r) override;

private:
    std::string tag_;
};

#endif  // HTTPP_LOG_HAS_ANDROID

#if HTTPP_LOG_HAS_UNIFIED

// Sends each record to Apple's unified logging (os_log) in `subsystem`, with the logger's name as the
// category, so Console.app and `log stream --predicate 'subsystem == "httpp"'` can pick a part of the
// library out. debug and info records are the OS's own debug and info types (kept in memory only,
// unless a profile or `log config` asks for more); warning is the default type, error is error and
// critical is fault. The text is logged as public: it is not redacted to "<private>". The default
// pattern is "%(message)s".
class unified_log_handler : public handler {
public:
    explicit unified_log_handler(std::string subsystem = "httpp");
    ~unified_log_handler() override;
    unified_log_handler(unified_log_handler&&) noexcept;
    unified_log_handler& operator=(unified_log_handler&&) noexcept;

protected:
    void emit(const record& r) override;

private:
    struct state;
    std::unique_ptr<state> st_;
};

#endif  // HTTPP_LOG_HAS_UNIFIED

#if HTTPP_LOG_HAS_DEBUGSTRING

// Sends each line to OutputDebugStringW: the output window of Visual Studio and any debugger, or
// Sysinternals DebugView when no debugger is attached. UTF-8 messages come out as they should
// (the text is converted to UTF-16 first). Nothing is written when nobody is listening, and the
// call costs next to nothing then. The default pattern is "[%(levelname)s] %(name)s: %(message)s":
// a debugger shows no level of its own.
class debug_string_handler : public handler {
public:
    debug_string_handler();

protected:
    void emit(const record& r) override;
};

#endif  // HTTPP_LOG_HAS_DEBUGSTRING

#if HTTPP_LOG_HAS_EVENTLOG

// Writes each record to the Windows Event Log (the Application log) as `source`, with the type
// information (debug, info), warning or error (error, critical), and the event id 1 + severity
// (1 debug ... 5 critical), so Event Viewer can filter by it. The default pattern is
// "%(name)s: %(message)s".
//
// Without a message file registered for the source (an installer does that, once, with administrator
// rights) Event Viewer shows the text after a note that "the description for event id N cannot be
// found". The text is all there. A message longer than 31,000 characters is cut. Throws
// std::runtime_error when the event source cannot be registered; a write that fails later is ignored.
class event_log_handler : public handler {
public:
    explicit event_log_handler(std::string source = "httpp");
    ~event_log_handler() override;
    event_log_handler(event_log_handler&&) noexcept;
    event_log_handler& operator=(event_log_handler&&) noexcept;

protected:
    void emit(const record& r) override;

private:
    struct state;
    std::unique_ptr<state> st_;
};

#endif  // HTTPP_LOG_HAS_EVENTLOG

}  // namespace httpp::logging
