#include "internal/logging.hpp"

#include "internal/fs.hpp"

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

void handler::handle(const record& r) {
    if (int(r.levelno) < st_->lv.load()) return;
    std::lock_guard<std::mutex> lock(st_->mu);
    for (const filter_fn& f : st_->filters)
        if (f && !f(r)) return;
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

file_handler::file_handler(const std::string& path, bool append)
    : out_(fs::native(path), append ? (std::ios::out | std::ios::app) : (std::ios::out | std::ios::trunc)) {
    if (!out_) throw std::runtime_error("cannot open the log file " + path);
}

void file_handler::emit(const record& r) {
    out_ << format(r) << '\n';
    out_.flush();
}

void file_handler::do_flush() { out_.flush(); }

callback_handler::callback_handler(std::function<void(const record&)> fn) : fn_(std::move(fn)) {}

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

void logger::set_level(level lv) noexcept { st_->lv.store(int(lv)); }
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

manager& instance() {
    static manager* m = [] {
        auto* p = new manager;
        try {
            p->get("httpp");  // the parent of every httpp logger exists from the start
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
