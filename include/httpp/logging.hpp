#pragma once

// The logging module (ADR-0050, made public by ADR-0057), modelled on Python's `logging`: a tree of dotted logger
// names, handlers (stream, file, callback, null, or your own class), a formatter with %(field)s patterns, levels
// with the same numbers as Python (plus trace = 5). Ported from libstb; it replaces the vendored AixLog.
// Python has the original: there it is the standard `logging` module, and the library's own "httpp" logger is
// already bridged to it (httpp/_log.py). This C++ module is for C++ programs, and it is what httpp itself logs through.
//
//   auto& lg = httpp::logging::get_logger("httpp");
//   lg.warning("backend %s is not available", name);        // printf style
//   lg.add_handler(httpp::logging::file_handler("httpp.log"));
//
// When no handler exists anywhere up the tree, warnings and worse still go to stderr ("last resort", as in Python).
// Threads: every call is thread safe, a handler writes one record at a time, and logging never throws.
// httpp's own lines go to the logger "httpp", which does not propagate to the root (the application's setup is
// never touched); httpp::set_log_level / set_log_file / set_log_system / set_log_callback (at the end of this header)
// drive it. By default warnings and errors go to stderr; quiet it with set_log_level(log_level::off), or send the
// lines to your own logger with set_log_callback().

#include "httpp/export.hpp"
#include "httpp/path.hpp"

#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>


#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251) // the std members of the exported classes are private and never touched by a caller
#endif

// ---- Handlers for the log of the operating system (ADR-0056, public since ADR-0075) ----
//
// So a program's records show up where its platform's own tools look: journalctl, `adb logcat`, the Console app,
// Visual Studio's output window and DebugView, Event Viewer. A class exists only on the platform that has its facility,
// and the macro beside it says so (1 or 0), so portable code can ask:
//
//     #if HTTPP_LOG_HAS_SYSLOG
//     httpp::logging::get_logger("httpp").add_handler(httpp::logging::syslog_handler("myapp"));
//     #endif
//
// They are handlers like any other: they have a level, filters and a formatter, they can be moved but not copied, and a
// failing OS call is never an error to the caller. The OS adds its own time, level and process to every line, so the
// default pattern leaves them out. The levels map to the five severities every one of these logs has: debug, info,
// warning, error, critical; a level in between (25) takes the one below it.

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

#if defined(__GNUC__) || defined(__clang__)
#define HTTPP_FORMAT_CHECK(format_index, first_value) __attribute__((format(printf, format_index, first_value)))
#define HTTPP_FORMAT_STRING
#elif defined(_MSC_VER)
#include <sal.h>
#define HTTPP_FORMAT_CHECK(format_index, first_value)
#define HTTPP_FORMAT_STRING _Printf_format_string_
#else
#define HTTPP_FORMAT_CHECK(format_index, first_value)
#define HTTPP_FORMAT_STRING
#endif


// ----------------------------------------------------------------------------------------- levels

enum class level : int { notset = 0, trace = 5, debug = 10, info = 20, warning = 30, error = 40, critical = 50 };

// "DEBUG", "INFO", ... ; "Level 15" for a number in between (as Python does).
HTTPP_API std::string level_name(level lv);

// "debug", "WARNING", "warn", "fatal" (case does not matter) or a whole number: 25 is a level between
// info and warning. Anything else gives nullopt.
HTTPP_API std::optional<level> level_from_string(std::string_view text);

// ---------------------------------------------------------------------------------------- records

struct record {
    std::string_view where;    // the function that logged (may be empty)
    std::string_view name;     // the logger's name ("root" for the root logger)
    level levelno;
    std::string_view message;  // already formatted
    std::chrono::system_clock::time_point created;
    std::thread::id thread;
};

// ------------------------------------------------------------------------------------- formatter

// A pattern with Python's %(field)x syntax: %(asctime)s %(created)f %(msecs)d %(levelname)s %(levelno)d
// %(name)s %(message)s %(thread)d %(process)d, and %% for a percent sign. A field may have a width and
// alignment, "%(levelname)-8s", and %(created) a precision, "%(created).3f". asctime is local time:
// "2026-11-04 10:38:12,345", or, when `datefmt` is given, strftime(datefmt) (no milliseconds). A pattern
// that is malformed or names an unknown field throws std::invalid_argument. Cheap to copy.
class HTTPP_API formatter {
public:
    static constexpr const char* default_pattern = "%(asctime)s [%(levelname)s] %(name)s: %(message)s";

    explicit formatter(std::string pattern = default_pattern, std::string datefmt = {});

    std::string format(const record& r) const;
    const std::string& pattern() const noexcept;

private:
    struct impl;
    std::shared_ptr<const impl> impl_;
};

// ------------------------------------------------------------------------------------- handlers

using filter_fn = std::function<bool(const record&)>;  // return false to drop the record

// The base of every handler. To write your own, derive and override emit():
//
//     struct my_handler : stb::logging::handler {
//         void emit(const stb::logging::record& r) override { /* use format(r) or r.message */ }
//     };
//
// emit() is called with the handler's lock held, only for a record that passed the level and the filters,
// so it needs no locking of its own. A handler can be moved but not copied. (The one exception is a
// callback_handler made with serialized = false, see below.)
class HTTPP_API handler {
public:
    handler();
    virtual ~handler();
    handler(handler&&) noexcept;
    handler& operator=(handler&&) noexcept;
    handler(const handler&) = delete;
    handler& operator=(const handler&) = delete;

    void set_level(level lv) noexcept;  // default notset: every record that reaches the handler
    level get_level() const noexcept;
    void set_formatter(formatter f);
    void add_filter(filter_fn f);

    void handle(const record& r);  // what a logger calls: level, filters, then emit() under the lock
    void flush();

protected:
    virtual void emit(const record& r) = 0;
    virtual void do_flush() {}
    std::string format(const record& r) const;  // with this handler's formatter (the default one if none)
    // false: handle() releases the lock before emit(), so several threads can be inside emit() at once and
    // emit() can log again. Such an emit() must do its own locking and must not call format() (it reads the
    // formatter that set_formatter() replaces under the lock). Set it once, before the handler is added.
    void set_serialized(bool on) noexcept;

private:
    struct state;
    std::unique_ptr<state> st_;
};

// Discards everything. Python's NullHandler: put one on a library's logger to say "no output unless the
// application configures it".
class HTTPP_API null_handler : public handler {
protected:
    void emit(const record&) override {}
};

enum class color_mode { automatic, always, never };

// One line per record on a C stream (stderr by default), flushed after every record. With
// color_mode::automatic the line is colored by level when the stream is a terminal and NO_COLOR is not
// set (never on Windows, where ANSI sequences need the console set up by the program: use always).
class HTTPP_API stream_handler : public handler {
public:
    explicit stream_handler(std::FILE* file = stderr, color_mode color = color_mode::automatic);

protected:
    void emit(const record& r) override;
    void do_flush() override;

private:
    std::FILE* file_;
    bool color_;
};

// One line per record appended to a file (append = false truncates it first), flushed after every record.
// Throws std::runtime_error when the file cannot be opened. (httpp::path, not std::filesystem::path: ADR-0054, 0055.)
class HTTPP_API file_handler : public handler {
public:
    explicit file_handler(const httpp::path& file, bool append = true);

protected:
    void emit(const record& r) override;
    void do_flush() override;

private:
    std::fstream out_; // opened with path::open
};

// Calls a function with each record (not formatted; format it with a formatter if you want a line). This
// is how the Python binding hands records to Python's logging.
//
// By default `fn` is called with the handler's lock held, one record at a time. That is a deadlock when `fn`
// has to wait for something that a logging thread holds while it logs: Python's GIL is the case that
// matters (a library thread waits in `fn` for the GIL with the lock held, while a Python thread that holds
// the GIL logs through the same handler and waits for the lock). Pass serialized = false and `fn` is called
// without the lock: nothing is held while it waits, `fn` may be running in several threads at once (it
// serializes itself, as taking the GIL does) and it may log again, so it must not log without a limit.
class HTTPP_API callback_handler : public handler {
public:
    explicit callback_handler(std::function<void(const record&)> fn, bool serialized = true);

protected:
    void emit(const record& r) override;

private:
    std::function<void(const record&)> fn_;
};

// ------------------------------------------------------------------------------------- loggers

namespace detail {
struct manager;
}

// Get loggers with get_logger(); they live until the program ends, so a reference stays valid.
class HTTPP_API logger {
public:
    ~logger();
    logger(const logger&) = delete;
    logger& operator=(const logger&) = delete;

    const std::string& name() const noexcept;
    logger* parent() const noexcept;  // nullptr for the root logger

    void set_level(level lv) noexcept;  // notset: use the parent's
    level get_level() const noexcept;
    level effective_level() const noexcept;  // the first level that is not notset, going up the tree
    bool is_enabled_for(level lv) const noexcept;

    void set_propagate(bool on) noexcept;  // default true: also pass records to the parents' handlers
    bool propagate() const noexcept;

    // Adds the handler (moved into the logger, which owns it) and returns a reference to it, valid until the
    // handler is removed or the logger is cleared: use it to set a level or a formatter, or to remove it.
    template <class H>
    H& add_handler(H h) {
        static_assert(std::is_base_of_v<handler, H>, "add_handler needs a class derived from stb::logging::handler");
        static_assert(std::is_move_constructible_v<H>, "the handler must be movable");
        auto owned = std::make_shared<H>(std::move(h));
        H& ref = *owned;
        add_owned(std::move(owned));
        return ref;
    }
    template <class H>
    void remove_handler(const H& h) {
        remove_owned(&h);
    }
    void clear_handlers();
    std::size_t handler_count() const;
    bool has_handlers() const;  // this logger or a parent it propagates to has one

    // printf style, like stb::sformat (the compiler checks the arguments). The text is built only when the
    // level is enabled; the arguments are still evaluated by the caller.
    HTTPP_FORMAT_CHECK(2, 3) void debug(HTTPP_FORMAT_STRING const char* fmt, ...);
    HTTPP_FORMAT_CHECK(2, 3) void info(HTTPP_FORMAT_STRING const char* fmt, ...);
    HTTPP_FORMAT_CHECK(2, 3) void warning(HTTPP_FORMAT_STRING const char* fmt, ...);
    HTTPP_FORMAT_CHECK(2, 3) void error(HTTPP_FORMAT_STRING const char* fmt, ...);
    HTTPP_FORMAT_CHECK(2, 3) void critical(HTTPP_FORMAT_STRING const char* fmt, ...);
    HTTPP_FORMAT_CHECK(3, 4) void log(level lv, HTTPP_FORMAT_STRING const char* fmt, ...);
    // A message already built, with the function that logged it (what httpp::detail::log_write uses).
    void log_from(level lv, std::string_view where, std::string_view message);
    HTTPP_FORMAT_CHECK(3, 0) void vlog(level lv, HTTPP_FORMAT_STRING const char* fmt, std::va_list args);

private:
    friend struct detail::manager;
    logger(std::string name, logger* parent);

    void add_owned(std::shared_ptr<handler> h);
    void remove_owned(const handler* h);
    void dispatch(const record& r);

    struct state;
    std::unique_ptr<state> st_;
};

// logging.getLogger(name). The same name gives the same logger. "" (or "root") is the root logger. Dotted
// names form the tree: "httpp.server" is a child of "httpp", which is a child of the root. The root's level
// is warning.
HTTPP_API logger& get_logger(std::string_view name = {});

// logging.basicConfig: when the root logger has no handler, adds a stream_handler on stderr with the default
// formatter; then sets the root level.
HTTPP_API void basic_config(level lv = level::warning);

// printf for std::string, used by the logger and the formatter.
namespace detail {

// Appends the text vsnprintf makes from (fmt, args) to `out`. Most results fit the stack buffer
// and cost one vsnprintf and one append; a longer one is measured by that first call, `out` is grown
// once to that size and the text is formatted straight into it.
inline void sformat_append(std::string& out, const char* fmt, std::va_list args) {
    char buf[256];
    std::va_list again;
    va_copy(again, args);
    struct end_list {
        std::va_list& l;
        ~end_list() { va_end(l); }
    } guard{again};
    const int n = std::vsnprintf(buf, sizeof buf, fmt, args);
    if (n < 0) return;  // an encoding error (for instance %ls): nothing is appended
    if (std::size_t(n) < sizeof buf) {
        out.append(buf, std::size_t(n));
        return;
    }
    const std::size_t old = out.size();
    out.resize(old + std::size_t(n));
    std::vsnprintf(&out[old], std::size_t(n) + 1, fmt, again);  // writes the terminating NUL at out[size()], which is allowed
}

}  // namespace detail

// printf for std::string: sformat("%dx%d (%s)", w, h, name). The format and its conversions are
// those of printf (%d %zu %lld %s %f %x %c %%, widths, precisions, `%.*s` for a string_view); the
// compiler warns about an argument that does not match its conversion on GCC and Clang. An encoding
// error gives an empty string rather than an exception; only std::bad_alloc can be thrown. Like
// printf it follows the C locale of the process, which httpp never changes (a program that sets
// LC_NUMERIC may get a comma in %f), and a null %s is undefined.
HTTPP_FORMAT_CHECK(1, 0)
inline std::string vsformat(HTTPP_FORMAT_STRING const char* fmt, std::va_list args) {
    std::string out;
    detail::sformat_append(out, fmt, args);
    return out;
}

HTTPP_FORMAT_CHECK(1, 2)
inline std::string sformat(HTTPP_FORMAT_STRING const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    struct end_list {
        std::va_list& l;
        ~end_list() { va_end(l); }
    } guard{args};
    return vsformat(fmt, args);
}

// The same, appended to the end of `out`: building a multi-part text costs no temporary string.
HTTPP_FORMAT_CHECK(2, 3)
inline void sformat_to(std::string& out, HTTPP_FORMAT_STRING const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    struct end_list {
        std::va_list& l;
        ~end_list() { va_end(l); }
    } guard{args};
    detail::sformat_append(out, fmt, args);
}


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
class HTTPP_API syslog_handler : public handler {
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
class HTTPP_API android_handler : public handler {
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
class HTTPP_API unified_log_handler : public handler {
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
class HTTPP_API debug_string_handler : public handler {
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
class HTTPP_API event_log_handler : public handler {
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

// ---- The library's own log (formerly httpp/log.hpp, ADR-0075) ----

namespace httpp {

enum class log_level { trace, debug, info, warning, error, off };

// Messages below this level are dropped. Thread-safe.
HTTPP_API void set_log_level(log_level level);

// Writes the log to this file (truncated) instead of stderr; "" goes back to stderr.
// Returns false (and keeps the previous output) if the file cannot be opened.
HTTPP_API bool set_log_file(const path& file);

// The log of the operating system, so the lines show up where the platform's own tools look: journalctl for
// syslog, `adb logcat`, the Console app for unified logging, Visual Studio's output window and DebugView, Event Viewer.
//   native        the usual one of this platform: android on Android, unified on Apple, debug_string on Windows,
//                 syslog on the other Unix systems
//   syslog        POSIX syslog(3) (Unix, macOS; not Android)
//   android       logcat
//   unified       Apple's unified logging, os_log (macOS and iOS, clang builds)
//   debug_string  Windows OutputDebugString
//   event_log     the Windows Event Log (Application), without a message file registered for the source
enum class log_system { native, syslog, android, unified, debug_string, event_log };

// Sends the log to the operating system's log instead of stderr, a file or a callback. `name` is what the system
// files the lines under (syslog ident, unified subsystem, event source; logcat uses the logger's name instead).
// The system adds its own time, level and process, so the lines carry only the function and the message.
// Returns false, and keeps the previous output, when this platform does not have `which` or it cannot be opened.
// syslog has one connection per process: the newest `name` wins for every syslog user in the program.
// Pass "" or call set_log_file("") to go back to stderr.
HTTPP_API bool set_log_system(log_system which = log_system::native, const std::string& name = "httpp");

// Sends every message that passes the level to `fn(level, where, message)`, replacing stderr/file
// output. `where` is the function that logged. `fn` may be called from any library thread, one call
// at a time, and must not log through httpp itself. An empty function restores stderr.
using log_callback = std::function<void(log_level level, const std::string& where, const std::string& message)>;
HTTPP_API void set_log_callback(log_callback fn);

} // namespace httpp

#ifdef _MSC_VER
#pragma warning(pop)
#endif
