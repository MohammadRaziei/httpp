#pragma once

// Logging for the library itself (not for your application). It uses a private logger of its own
// (ADR-0050): it does not touch std::clog or your own logging setup.
//
// By default warnings and errors go to stderr. Quiet it with set_log_level(log_level::off), or send
// the lines to your own logger with set_log_callback().

#include "httpp/export.hpp"
#include "httpp/path.hpp"

#include <functional>
#include <string>

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
