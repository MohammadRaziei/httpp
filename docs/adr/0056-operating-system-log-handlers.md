# ADR-0056: The operating system log handlers of libstb, behind `set_log_system`

- **Status:** accepted
- **Decided by:** the project owner ("yes, port them"), after the comparison with libstb's git log
- **Source:** `src/core/internal/logging_os.hpp`, `src/core/logging_os.cpp`, `include/httpp/log.hpp`, `tests/cpp/test_logging_os.cpp`, `tests/python/test_log.py`

## Context

ADR-0050 ported libstb's logging module but left out its handlers for the log of the operating system (libstb added them later). A server run as a service wants its lines in journalctl, logcat, Console.app or Event Viewer.

## Decision

- The five handlers are ported as they are (same classes, same level mapping: five severities, a level in between takes the one below): `syslog_handler`, `android_handler`, `unified_log_handler`, `debug_string_handler`, `event_log_handler`. They live in the internal logging module (`internal/logging_os.hpp`, `logging_os.cpp`), each only on the platform that has its facility (`HTTPP_LOG_HAS_*`). The one libstb helper that was a separate header (`logging_os_rank.hpp`) is folded into that header, and `stb::io_error` became `std::runtime_error`.
- One public function, the same in both languages, like `set_log_file`: C++ `set_log_system(log_system which = native, const std::string& name = "httpp")`, Python `set_log_system(which="native", name="httpp")` with `which` one of `"native"`, `"syslog"`, `"android"`, `"unified"`, `"debug_string"`, `"event_log"`. `native` is android on Android, unified on Apple, debug_string on Windows and syslog on the other Unix systems. It replaces the current output (stderr, a file, a callback) like the other setters.
- A system the platform does not have, or one that cannot be opened, returns false in C++ (the previous output is kept) and raises `OSError` in Python; an unknown name is a `ValueError`.
- The system adds time, level and process, so the line carries only `funcName: message` (debug_string, which shows no level, keeps `[LEVEL]`).
- syslog has one connection per process: its old handler is removed before the new `openlog()`, so a second call does not end up with `closelog()` after it.
- The handler classes stay internal: no public handler API. If someone needs to combine outputs (a file and syslog at once) that is a separate decision.

## Consequences

- Only syslog could be run here (Linux): the tests feed every level to it and check the switching and the refusals. What journald, logcat, the unified log, the debugger output and the Event Log do with a record cannot be read back by a test, and the Android, Apple and Windows code was not compiled on those systems. CI is the first test of it.
- Windows links `advapi32`, Android links `log` (CMake).

## Related

ADR-0050, ADR-0054
