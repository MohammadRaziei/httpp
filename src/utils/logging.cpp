#include "httpp/logging.hpp"
#include "internal/logging.hpp"


// System headers of the OS handlers. Their macros stay in this file, and the Windows ones are pruned right away
// (NOGDI keeps ERROR, NOMINMAX min/max out; the #undefs below are a second guard for what a build still defines).
#if HTTPP_LOG_HAS_SYSLOG
#include <syslog.h>
#endif
#if HTTPP_LOG_HAS_ANDROID
#include <android/log.h>
#endif
#if HTTPP_LOG_HAS_UNIFIED
#include <os/log.h>
#include <unordered_map>
#endif
#if HTTPP_LOG_HAS_DEBUGSTRING || HTTPP_LOG_HAS_EVENTLOG
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif
#include <climits>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif


namespace httpp::logging {

// ----------------------------------------------------------------------------------------- levels

std::string level_name(level lv) {
    switch (lv) {
        case level::notset: return "NOTSET";
        case level::trace: return "TRACE";
        case level::debug: return "DEBUG";
        case level::info: return "INFO";
        case level::warning: return "WARNING";
        case level::error: return "ERROR";
        case level::critical: return "CRITICAL";
        case level::off: return "OFF";
    }
    return sformat("Level %d", int(lv));
}

std::optional<level> level_from_string(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text.empty()) return std::nullopt;

    std::string low(text);
    for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (low == "notset") return level::notset;
    if (low == "debug") return level::debug;
    if (low == "info") return level::info;
    if (low == "warning" || low == "warn") return level::warning;
    if (low == "error") return level::error;
    if (low == "trace") return level::trace;
    if (low == "critical" || low == "fatal") return level::critical;
    if (low == "off") return level::off;

    if (low.size() > 4 || !std::all_of(low.begin(), low.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    return level(std::atoi(low.c_str()));
}

// ------------------------------------------------------------------------------------- formatter

namespace {

int process_id() {
#ifdef _WIN32
    return _getpid();
#else
    return int(::getpid());
#endif
}

std::string asctime_string(const record& r, const std::string& datefmt) {
    const std::time_t t = std::chrono::system_clock::to_time_t(r.created);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    const char* fmt = datefmt.empty() ? "%Y-%m-%d %H:%M:%S" : datefmt.c_str();
    std::string buf(128, '\0');
    std::size_t n = 0;
    while ((n = std::strftime(&buf[0], buf.size(), fmt, &tm)) == 0 && buf.size() < 4096) buf.assign(buf.size() * 2, '\0');
    buf.resize(n);
    if (datefmt.empty()) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(r.created.time_since_epoch()).count() % 1000;
        sformat_to(buf, ",%03d", int(ms));
    }
    return buf;
}

}  // namespace

struct formatter::impl {
    enum class field { literal, asctime, created, msecs, levelname, levelno, name, message, thread, process, funcname };
    struct token {
        field f = field::literal;
        std::string text;  // the literal text
        bool left = false;
        bool zero = false;
        int width = 0;
        int prec = -1;
        char conv = 's';
    };

    std::string pattern;
    std::string datefmt;
    std::vector<token> tokens;

    static field field_from(std::string_view n, char& natural_conv) {
        struct row {
            const char* name;
            field f;
            char conv;
        };
        static const row rows[] = {{"asctime", field::asctime, 's'},   {"created", field::created, 'f'},
                                   {"msecs", field::msecs, 'd'},       {"levelname", field::levelname, 's'},
                                   {"levelno", field::levelno, 'd'},   {"name", field::name, 's'},
                                   {"message", field::message, 's'},   {"thread", field::thread, 'd'},
                                   {"process", field::process, 'd'}, {"funcName", field::funcname, 's'}};
        for (const row& r : rows)
            if (n == r.name) {
                natural_conv = r.conv;
                return r.f;
            }
        throw std::invalid_argument("httpp::logging::formatter: unknown field %(" + std::string(n) + ")");
    }

    void parse() {
        std::string literal;
        auto flush_literal = [&] {
            if (literal.empty()) return;
            token t;
            t.text = std::move(literal);
            tokens.push_back(std::move(t));
            literal.clear();
        };
        const std::string_view p = pattern;
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (p[i] != '%') {
                literal += p[i];
                continue;
            }
            if (++i >= p.size()) throw std::invalid_argument("httpp::logging::formatter: the pattern ends with a lone %");
            if (p[i] == '%') {
                literal += '%';
                continue;
            }
            if (p[i] != '(')
                throw std::invalid_argument("httpp::logging::formatter: % must be followed by (field) or by %");
            const std::size_t close = p.find(')', i);
            if (close == std::string_view::npos)
                throw std::invalid_argument("httpp::logging::formatter: missing ) after %(");

            token t;
            char natural = 's';
            t.f = field_from(p.substr(i + 1, close - i - 1), natural);
            std::size_t j = close + 1;
            for (; j < p.size() && (p[j] == '-' || p[j] == '0'); ++j) (p[j] == '-' ? t.left : t.zero) = true;
            while (j < p.size() && p[j] >= '0' && p[j] <= '9') t.width = std::min(t.width * 10 + (p[j++] - '0'), 1000);
            if (j < p.size() && p[j] == '.') {
                ++j;
                t.prec = 0;
                while (j < p.size() && p[j] >= '0' && p[j] <= '9') t.prec = std::min(t.prec * 10 + (p[j++] - '0'), 1000);
            }
            if (j >= p.size()) throw std::invalid_argument("httpp::logging::formatter: a field needs s, d or f after it");
            t.conv = p[j];
            const bool ok = t.conv == natural || (t.f == field::created && t.conv == 'd');
            if (!ok)
                throw std::invalid_argument("httpp::logging::formatter: a field cannot be written with %" +
                                            std::string(1, t.conv) + " here");
            i = j;
            flush_literal();
            tokens.push_back(std::move(t));
        }
        flush_literal();
    }

    static void pad(std::string& out, std::string value, const token& t) {
        if (t.conv == 's' && t.prec >= 0 && std::size_t(t.prec) < value.size()) value.resize(std::size_t(t.prec));
        const std::size_t width = std::size_t(t.width);
        if (value.size() >= width) {
            out += value;
            return;
        }
        const std::size_t fill = width - value.size();
        if (t.left) {
            out += value;
            out.append(fill, ' ');
        } else if (t.zero && t.conv != 's') {
            const std::size_t sign = (!value.empty() && value[0] == '-') ? 1 : 0;
            out.append(value, 0, sign);
            out.append(fill, '0');
            out.append(value, sign, std::string::npos);
        } else {
            out.append(fill, ' ');
            out += value;
        }
    }

    std::string run(const record& r) const {
        std::string out;
        out.reserve(pattern.size() + r.message.size() + 32);
        for (const token& t : tokens) {
            std::string v;
            switch (t.f) {
                case field::literal: out += t.text; continue;
                case field::asctime: v = asctime_string(r, datefmt); break;
                case field::created: {
                    const double secs = std::chrono::duration<double>(r.created.time_since_epoch()).count();
                    v = t.conv == 'f' ? sformat("%.*f", t.prec >= 0 ? t.prec : 6, secs) : std::to_string((long long)secs);
                    break;
                }
                case field::msecs:
                    v = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(r.created.time_since_epoch()).count() % 1000);
                    break;
                case field::levelname: v = level_name(r.levelno); break;
                case field::levelno: v = std::to_string(int(r.levelno)); break;
                case field::name: v.assign(r.name); break;
                case field::message: v.assign(r.message); break;
                case field::thread:
                    v = std::to_string((unsigned long long)std::hash<std::thread::id>{}(r.thread));
                    break;
                case field::process: v = std::to_string(process_id()); break;
                case field::funcname: v.assign(r.where); break;
            }
            pad(out, std::move(v), t);
        }
        return out;
    }
};

formatter::formatter(std::string pattern, std::string datefmt) {
    auto p = std::make_shared<impl>();
    p->pattern = std::move(pattern);
    p->datefmt = std::move(datefmt);
    p->parse();
    impl_ = std::move(p);
}

std::string formatter::format(const record& r) const { return impl_->run(r); }
const std::string& formatter::pattern() const noexcept { return impl_->pattern; }

// ------------------------------------------------------------------------------------- handlers

struct handler::state {
    std::atomic<int> lv{int(level::notset)};
    std::atomic<bool> serialized{true};  // emit() under `mu` (default), or after it is released
    std::mutex mu;
    formatter fmt;
    std::vector<filter_fn> filters;
};

handler::handler() : st_(std::make_unique<state>()) {}
handler::~handler() = default;
handler::handler(handler&&) noexcept = default;
handler& handler::operator=(handler&&) noexcept = default;

void handler::set_level(level lv) noexcept { st_->lv.store(int(lv)); }
level handler::get_level() const noexcept { return level(st_->lv.load()); }

void handler::set_formatter(formatter f) {
    std::lock_guard<std::mutex> lock(st_->mu);
    st_->fmt = std::move(f);
}

void handler::add_filter(filter_fn f) {
    std::lock_guard<std::mutex> lock(st_->mu);
    st_->filters.push_back(std::move(f));
}

void handler::set_serialized(bool on) noexcept { st_->serialized.store(on); }

void handler::handle(const record& r) {
    if (int(r.levelno) < st_->lv.load()) return;
    std::unique_lock<std::mutex> lock(st_->mu);
    for (const filter_fn& f : st_->filters)
        if (f && !f(r)) return;
    if (!st_->serialized.load()) lock.unlock();  // the filters ran under the lock; emit() waits for nobody's lock
    emit(r);
}

void handler::flush() {
    std::lock_guard<std::mutex> lock(st_->mu);
    do_flush();
}

std::string handler::format(const record& r) const { return st_->fmt.format(r); }

namespace {

bool wants_color(std::FILE* file, color_mode mode) {
    if (mode == color_mode::always) return true;
    if (mode == color_mode::never || !file) return false;
#ifdef _WIN32
    return false;
#else
    const char* no_color = std::getenv("NO_COLOR");
    if (no_color && *no_color) return false;
    return ::isatty(::fileno(file)) != 0;
#endif
}

const char* color_code(level lv) {
    if (int(lv) >= int(level::critical)) return "1;31";
    if (int(lv) >= int(level::error)) return "31";
    if (int(lv) >= int(level::warning)) return "33";
    if (int(lv) >= int(level::info)) return "32";
    return "36";
}

}  // namespace

stream_handler::stream_handler(std::FILE* file, color_mode color) : file_(file), color_(wants_color(file, color)) {}

void stream_handler::emit(const record& r) {
    if (!file_) return;
    std::string line;
    if (color_) {
        line = "\x1b[";
        line += color_code(r.levelno);
        line += 'm';
        line += format(r);
        line += "\x1b[0m\n";
    } else {
        line = format(r);
        line += '\n';
    }
    std::fwrite(line.data(), 1, line.size(), file_);
    std::fflush(file_);
}

void stream_handler::do_flush() {
    if (file_) std::fflush(file_);
}

file_handler::file_handler(const httpp::path& file, bool append)
    : out_(file.open(append ? (std::ios::out | std::ios::app) : (std::ios::out | std::ios::trunc))) {
    if (!out_) throw std::runtime_error("cannot open the log file " + file.str());
}

void file_handler::emit(const record& r) {
    out_ << format(r) << '\n';
    out_.flush();
}

void file_handler::do_flush() { out_.flush(); }

callback_handler::callback_handler(std::function<void(const record&)> fn, bool serialized) : fn_(std::move(fn)) {
    set_serialized(serialized);
}

void callback_handler::emit(const record& r) {
    if (fn_) fn_(r);
}

// ------------------------------------------------------------------------------------- loggers

struct logger::state {
    std::string name;
    std::atomic<logger*> parent{nullptr};
    std::atomic<int> lv{int(level::notset)};
    std::atomic<bool> propagate{true};
    mutable std::mutex mu;
    std::vector<std::shared_ptr<handler>> handlers;
};

logger::logger(std::string name, logger* parent) : st_(std::make_unique<state>()) {
    st_->name = std::move(name);
    st_->parent.store(parent);
}
logger::~logger() = default;

const std::string& logger::name() const noexcept { return st_->name; }
logger* logger::parent() const noexcept { return st_->parent.load(); }

void logger::set_level(level lv) noexcept {
    st_->lv.store(int(lv));
    detail::level_epoch.fetch_add(1, std::memory_order_release);  // every cached effective level is stale now
}

bool logger::enabled_slow(level lv) const noexcept {
    // The epoch is read before the level: a set_level() that lands in between changes the epoch, so the cache stored here is never
    // taken for the newer state.
    const std::uint32_t epoch = detail::level_epoch.load(std::memory_order_acquire);
    const int effective = static_cast<int>(effective_level());
    cache_.store((static_cast<std::uint64_t>(epoch) << 32) | static_cast<std::uint32_t>(static_cast<std::int32_t>(effective)),
                 std::memory_order_relaxed);
    return static_cast<int>(lv) >= effective;
}
level logger::get_level() const noexcept { return level(st_->lv.load()); }

level logger::effective_level() const noexcept {
    for (const logger* l = this; l; l = l->st_->parent.load()) {
        const int v = l->st_->lv.load();
        if (v != int(level::notset)) return level(v);
    }
    return level::notset;
}

bool logger::is_enabled_for(level lv) const noexcept { return int(lv) >= int(effective_level()); }

void logger::set_propagate(bool on) noexcept { st_->propagate.store(on); }
bool logger::propagate() const noexcept { return st_->propagate.load(); }

void logger::add_owned(std::shared_ptr<handler> h) {
    std::lock_guard<std::mutex> lock(st_->mu);
    st_->handlers.push_back(std::move(h));
}

void logger::remove_owned(const handler* h) {
    std::lock_guard<std::mutex> lock(st_->mu);
    st_->handlers.erase(std::remove_if(st_->handlers.begin(), st_->handlers.end(),
                                       [h](const std::shared_ptr<handler>& p) { return p.get() == h; }),
                        st_->handlers.end());
}

void logger::clear_handlers() {
    std::lock_guard<std::mutex> lock(st_->mu);
    st_->handlers.clear();
}

std::size_t logger::handler_count() const {
    std::lock_guard<std::mutex> lock(st_->mu);
    return st_->handlers.size();
}

bool logger::has_handlers() const {
    for (const logger* l = this; l; l = l->st_->parent.load()) {
        if (l->handler_count() > 0) return true;
        if (!l->st_->propagate.load()) break;
    }
    return false;
}

namespace {

// Python's "last resort": with no handler anywhere, warnings and worse still reach the user on stderr.
void last_resort(const record& r) {
    static stream_handler* h = [] {
        auto* p = new stream_handler(stderr, color_mode::never);  // kept for the life of the program on purpose
        p->set_level(level::warning);
        p->set_formatter(formatter("%(name)s: %(message)s"));
        return p;
    }();
    h->handle(r);
}

}  // namespace

void logger::dispatch(const record& r) {
    int found = 0;
    for (logger* l = this; l; l = l->st_->parent.load()) {
        std::vector<std::shared_ptr<handler>> handlers;
        {
            std::lock_guard<std::mutex> lock(l->st_->mu);
            handlers = l->st_->handlers;  // a copy: a handler removed meanwhile stays alive until we are done
        }
        for (const std::shared_ptr<handler>& h : handlers) {
            ++found;
            try {
                h->handle(r);
            } catch (...) {
                // like Python: a failing handler must not break the program that logs
            }
        }
        if (!l->st_->propagate.load()) break;
    }
    if (found == 0 && int(r.levelno) >= int(level::warning)) {
        try {
            last_resort(r);
        } catch (...) {
        }
    }
}

void logger::log_from(level lv, std::string_view where, std::string_view message) {
    try {
        if (!is_enabled_for(lv)) return;
        dispatch(record{where, st_->name, lv, message, std::chrono::system_clock::now(), std::this_thread::get_id()});
    } catch (...) {
    }
}

void logger::vlog(level lv, const char* fmt, std::va_list args) {
    try {
        if (!is_enabled_for(lv)) return;
        const std::string message = vsformat(fmt, args);
        const record r{{}, st_->name, lv, message, std::chrono::system_clock::now(), std::this_thread::get_id()};
        dispatch(r);
    } catch (...) {
    }
}

#define STB_LOGGER_METHOD(method, lv)        \
    void logger::method(const char* fmt, ...) { \
        std::va_list args;                    \
        va_start(args, fmt);                  \
        vlog(lv, fmt, args);                  \
        va_end(args);                         \
    }
STB_LOGGER_METHOD(debug, level::debug)
STB_LOGGER_METHOD(info, level::info)
STB_LOGGER_METHOD(warning, level::warning)
STB_LOGGER_METHOD(error, level::error)
STB_LOGGER_METHOD(critical, level::critical)
#undef STB_LOGGER_METHOD

void logger::log(level lv, const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    vlog(lv, fmt, args);
    va_end(args);
}

// -------------------------------------------------------------------------------------- registry

namespace detail {

// Python's Logger.manager: name -> logger. It lives in the library (never in a header, which would give
// every shared object its own copy) and is never destroyed, so code that logs while the program exits
// cannot meet a dead logger.
struct manager {
    std::mutex mu;
    std::unique_ptr<logger> root;
    std::map<std::string, std::unique_ptr<logger>, std::less<>> loggers;

    manager() {
        root.reset(new logger("root", nullptr));
        root->set_level(level::warning);
    }

    logger& get(std::string_view name) {
        if (name.empty() || name == "root") return *root;
        std::lock_guard<std::mutex> lock(mu);
        const auto found = loggers.find(name);
        if (found != loggers.end()) return *found->second;

        // The parent is the longest existing dotted prefix, else the root.
        logger* parent = root.get();
        for (std::size_t pos = name.rfind('.'); pos != std::string_view::npos;
             pos = (pos == 0 ? std::string_view::npos : name.rfind('.', pos - 1))) {
            const auto up = loggers.find(name.substr(0, pos));
            if (up != loggers.end()) {
                parent = up->second.get();
                break;
            }
        }
        std::unique_ptr<logger> made(new logger(std::string(name), parent));
        logger& ref = *made;

        // Loggers created earlier below this name (a child made before its parent) now hang from it.
        const std::string prefix = std::string(name) + ".";
        for (auto& entry : loggers) {
            if (entry.first.compare(0, prefix.size(), prefix) != 0) continue;
            logger* current = entry.second->st_->parent.load();
            if (current == root.get() || current->st_->name.size() < name.size()) entry.second->st_->parent.store(&ref);
        }
        loggers.emplace(std::string(name), std::move(made));
        return ref;
    }
};

namespace {

// The output format of the library's own log (set_log_file and the others reuse it).
constexpr const char* k_lib_pattern = "%(asctime)s [%(levelname)s] %(name)s (%(funcName)s): %(message)s";

manager& instance() {
    static manager* m = [] {
        auto* p = new manager;
        try {
            // The library's own logger exists from the start, with its defaults (ADR-0011): `warning`, to stderr, never to the application's
            // root. Whoever asks for get_logger("httpp") first (the core, a program, a test) gets it already set up. Nothing in here may
            // call get_logger(): this initializer is still running.
            logger& lib = p->get("httpp");
            lib.set_level(level::warning);
            lib.set_propagate(false);
            stream_handler h(stderr);
            h.set_formatter(formatter(k_lib_pattern));
            lib.add_handler(std::move(h));
        } catch (...) {
        }
        return p;
    }();
    return *m;
}

}  // namespace
}  // namespace detail

logger& get_logger(std::string_view name) { return detail::instance().get(name); }

void basic_config(level lv) {
    logger& root = get_logger();
    if (root.handler_count() == 0) root.add_handler(stream_handler());
    root.set_level(lv);
}

}  // namespace httpp::logging

// ---- Handlers for the log of the operating system (ADR-0011) ----

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

// ---- The library's own log: set_log_* and the HTTPP_LOG back end (formerly src/core/log.cpp) ----

namespace httpp {
namespace {

constexpr const char* k_pattern = logging::detail::k_lib_pattern;

struct state {
    std::mutex mu;                                 // swapping the output, so a line never meets a half-changed setup
    logging::logger* lg;

    state() : lg(&logging::get_logger("httpp")) {}  // already set up by the registry (see instance())
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

void set_log_level(logging::level level) { st().lg->set_level(level); }

bool set_log_level(std::string_view name) {
    const auto parsed = logging::level_from_string(name);  // the one table of names
    if (!parsed) return false;
    set_log_level(*parsed);
    return true;
}

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
        fn(logging::level(r.levelno), std::string(r.where), std::string(r.message));
    }));
}

} // namespace httpp

namespace httpp::logging::detail {
std::atomic<std::uint32_t> level_epoch{1};
}  // namespace httpp::logging::detail

namespace httpp {
int utils_abi_version() noexcept { return HTTPP_UTILS_ABI_VERSION; }
}  // namespace httpp
