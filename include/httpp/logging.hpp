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
// never touched); httpp::set_log_level / set_log_file / set_log_system / set_log_callback (httpp/log.hpp) drive it.

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
// so it needs no locking of its own. A handler can be moved but not copied.
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
class HTTPP_API callback_handler : public handler {
public:
    explicit callback_handler(std::function<void(const record&)> fn);

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

}  // namespace httpp::logging

#ifdef _MSC_VER
#pragma warning(pop)
#endif
