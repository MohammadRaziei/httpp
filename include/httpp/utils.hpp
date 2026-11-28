#pragma once

// The small helpers of httpp_utils that need nothing but the standard library and the operating system (ADR-0086, ADR-0107): SHA-256
// and environment variables. They share this header and src/utils/utils.cpp; the bigger parts of the library (logging, path,
// progress) keep a header of their own.

#include "httpp/export.hpp"
#include "httpp/path.hpp"

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

namespace httpp {

// ---- SHA-256 -------------------------------------------------------------------------------------------------------------------

// SHA-256 of `size` bytes, as 64 lowercase hex characters.
// ponytail: portable code (no TLS library: httpp_utils links none, so a program that only hashes files does not carry the HTTP stack).
// Upgrade path if hashing 100 MB+ files proves slow: the SHA-NI (x86) / ARMv8 crypto routines, picked at run time, ~4x faster.
HTTPP_UTILS_API std::string sha256_hex(const void* data, std::size_t size);

// Same, for a whole file, read in chunks (memory use does not depend on the file size).
// Throws std::runtime_error if the file cannot be opened or read.
HTTPP_UTILS_API std::string sha256_file(const path& file);

// Parses the text of a checksum file: one line, `<64 hex digits>`, optionally followed by whitespace, an optional
// `*` and the file name, as `sha256sum` writes it. If a name is there it must equal `file_name`. Returns the digest
// in lowercase. Anything else (several lines, a short line, an HTML error page, ...) throws std::runtime_error.
HTTPP_UTILS_API std::string parse_sha256_checksum(const std::string& text, const std::string& file_name);

} // namespace httpp

// ---- environment variables ------------------------------------------------------------------------------------------------------

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

// ---- the end of a thread httpp started -------------------------------------------------------------------------------------------

namespace httpp {

// A function that httpp calls on every thread it started (worker pool threads, I/O threads, the network thread of an async client, the
// thread of an asynchronous download), on that thread, as the very last thing it does before the thread ends. It is for a program that
// keeps state for a thread it did not start and must give it back when the thread goes: the Python binding keeps a Python thread state
// for each such thread that runs Python code, and frees it here. Not called for a thread the caller started itself.
//
// It runs in the thread function, never from a destructor of a thread_local or from DllMain, so it may block (the Python binding waits
// for its interpreter there). It must not throw. One function for the whole process; nullptr removes it.
using thread_exit_fn = void (*)() noexcept;
HTTPP_UTILS_API void set_thread_exit_hook(thread_exit_fn fn) noexcept;

// Calls the function that set_thread_exit_hook() installed, if any. httpp's own threads call it; so can a program that starts a thread
// of its own and wants the same cleanup for it.
HTTPP_UTILS_API void run_thread_exit_hook() noexcept;

} // namespace httpp
