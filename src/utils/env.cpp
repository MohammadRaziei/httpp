#include "httpp/env.hpp"

#include "internal/windows_text.hpp" // widen(), narrow()

#include <cstdlib>
#include <mutex>

namespace httpp::env {

namespace {

// One lock for every call here, in this module only: libhttpp_utils is one shared library, so there is one lock per process.
std::mutex& environment_lock() {
    static std::mutex lock;
    return lock;
}

} // namespace

#ifdef _WIN32

// ---- Windows: the CRT's wide environment (getenv() reads the same copy that _wputenv_s() writes) ------------------

std::optional<std::string> get(const std::string& name) {
    const std::wstring wide_name = detail::widen(name);
    std::lock_guard<std::mutex> hold(environment_lock());
    std::size_t needed = 0; // with the terminating null; 0 when the variable is not set
    if (_wgetenv_s(&needed, nullptr, 0, wide_name.c_str()) != 0 || needed == 0) return std::nullopt;
    std::wstring value(needed, L'\0');
    if (_wgetenv_s(&needed, &value[0], value.size(), wide_name.c_str()) != 0 || needed == 0) return std::nullopt;
    value.resize(needed - 1);
    return detail::narrow(value);
}

bool set(const std::string& name, const std::string& value) {
    const std::wstring wide_name = detail::widen(name);
    const std::wstring wide_value = detail::widen(value);
    std::lock_guard<std::mutex> hold(environment_lock());
    return _wputenv_s(wide_name.c_str(), wide_value.c_str()) == 0; // an empty value removes the variable
}

bool unset(const std::string& name) { return set(name, std::string()); }

#else

// ---- POSIX -----------------------------------------------------------------------------------------------------

std::optional<std::string> get(const std::string& name) {
    std::lock_guard<std::mutex> hold(environment_lock());
    const char* value = std::getenv(name.c_str());
    if (!value) return std::nullopt;
    return std::string(value); // copied under the lock: a later set() may free what getenv() pointed to
}

bool set(const std::string& name, const std::string& value) {
    std::lock_guard<std::mutex> hold(environment_lock());
    return ::setenv(name.c_str(), value.c_str(), 1) == 0; // EINVAL for an empty name or one with '='
}

bool unset(const std::string& name) {
    std::lock_guard<std::mutex> hold(environment_lock());
    return ::unsetenv(name.c_str()) == 0;
}

#endif

// ---- the same everywhere ---------------------------------------------------------------------------------------

std::string first(std::initializer_list<const char*> names) {
    for (const char* name : names)
        if (auto value = get(name); value && !value->empty()) return std::move(*value);
    return std::string();
}

scope::scope(const std::string& name, const std::string& value) : name_(name), previous_(get(name)) { set(name_, value); }

scope::scope(const std::string& name) : name_(name), previous_(get(name)) { unset(name_); }

bool scope::restore() {
    if (!std::exchange(active_, false)) return false;
    return previous_ ? set(name_, *previous_) : unset(name_);
}

} // namespace httpp::env
