#pragma once

// Environment variables, the way Python's os.environ and os.getenv give them (ADR-0107). Names and values are UTF-8 on every platform:
// on Windows each call goes through the wide API, so a value in a user's own script (a proxy with a non-Latin user name) is not turned
// into the code page. A name that is empty or has an '=' in it is never a variable: it is not found, and set() and unset() say false.
//
//     if (auto home = httpp::env::get("HOME")) use(*home);          // nullopt: not set. An empty value is a value (see Windows below)
//     std::string t = httpp::env::first({"TMPDIR", "TMP", "TEMP"}); // the first that is set and not empty, else ""
//     httpp::env::set("HTTP_PROXY", "http://p:3128");   httpp::env::unset("HTTP_PROXY");
//     { httpp::env::scope s("HTTP_PROXY", "http://p:3128"); ... } // the old value, or its absence, is back after the block
//
// Threads. The calls here are serialized by one lock, and get() copies the value under it, so a value read here is never torn by a
// set() made here. The C library gives no such promise to anyone else: a getenv() made directly (by the library or by other code)
// while set() runs is undefined on POSIX, and the lock cannot hold it back. Set variables before starting threads, and read them
// through get() or first(). scope() is for tests and for a program's first lines; it is not a way to pass values between threads.
//
// Windows has no variable that is set and empty: set(name, "") removes it, so get() then says nullopt where POSIX says "".

#include "httpp/export.hpp"

#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

namespace httpp::env {

// The value, or nullopt when the variable is not set.
HTTPP_UTILS_API std::optional<std::string> get(const std::string& name);

// The value of the first of the names that is set and not empty, else "": the way curl reads http_proxy before all_proxy, and TMPDIR before TMP.
HTTPP_UTILS_API std::string first(std::initializer_list<const char*> names);

// Sets (replacing) or removes a variable of this process. True when it was done.
HTTPP_UTILS_API bool set(const std::string& name, const std::string& value);
HTTPP_UTILS_API bool unset(const std::string& name);

// A variable changed for as long as the object lives: the previous value (or its absence) comes back in the destructor. Movable, so
// several can be kept in a container; not copyable.
//
//     env::scope proxy("http_proxy", "http://p:3128");  // set
//     env::scope none("NO_PROXY");                      // removed
class scope {
public:
    HTTPP_UTILS_API scope(const std::string& name, const std::string& value);
    HTTPP_UTILS_API explicit scope(const std::string& name); // removes the variable
    scope(const scope&) = delete;
    scope& operator=(const scope&) = delete;
    scope& operator=(scope&&) = delete;
    scope(scope&& other) noexcept : name_(std::move(other.name_)), previous_(std::move(other.previous_)), active_(std::exchange(other.active_, false)) {}
    ~scope() { restore(); }

    // Puts the old state back now (the destructor does it too, once). False when there was nothing to put back or it could not be.
    HTTPP_UTILS_API bool restore();

private:
    std::string name_;
    std::optional<std::string> previous_;
    bool active_ = true;
};

} // namespace httpp::env
