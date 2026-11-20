# ADR-0011: Logging: one public module, one level type, one logger for the library, a bridge to Python

- **Status:** accepted (merged ADR; each part keeps its own status line)
- **Merged on:** 2026-11-20, at the request of the project owner (the ADRs of one subject were spread over many numbers). The text of every part is kept; the old numbers still name the parts below.
- **Merges:** ADR-0011, ADR-0018, ADR-0050, ADR-0056, ADR-0057, ADR-0075, ADR-0080, ADR-0085, ADR-0090, ADR-0091

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history, in the order they were decided.

- **One module:** `httpp::logging` is public (`httpp/logging.hpp`, one header, one source `logging.cpp`): loggers, handlers (file, stream, callback, and the five operating system handlers), formatter, levels. It was ported from libstb and replaced AixLog (0050), made public (0057), and folded into one header and one source together with the old `httpp/log.hpp` and the OS handlers (0075, 0056).
- **One level type:** `httpp::logging::level`, with `off = 60`. The old `log_level` enum is gone (0085, supersedes the enum of 0075).
- **The library's own log is the logger `"httpp"`.** Its level is that logger's level (default `warning`), `set_log_level`, `set_log_file`, `set_log_system` and `set_log_callback` are thin helpers on it, and the internal macro `HTTPP_LIB_LOG` is only an alias of the public one (0091, 0075).
- **Cheap disabled lines:** `HTTPP_LOG(log, level) << ...` (streams) and `HTTPP_LOGF(log, level, fmt, ...)` (printf) skip their arguments when the level is off; `logger::enabled()` is an inline cached check (0090).
- **What is logged by default:** one `info` line per response and `listening on http://host:port` after a successful bind. `run` and the commands `httpp server` / `httpp browse` default to `info`; `listen()` and the C++ API stay at `warning` (0011).
- **Python:** the module has no binding (Python's `logging` is its counterpart, an intentional exception to the dual-API rule). The `"httpp"` logger is bridged into `logging.getLogger("httpp")` and the Python logger decides the C++ level (0018). `httpp.bridge_logger(name)` does the same for any other C++ logger tree, for example libspeech (0080).
- **Superseded inside this group:** the AixLog back end named in the first part (replaced in 0050); the `log_level` enum (0085); "the OS handlers stay internal" in 0056 and 0057 (public since 0075); the parallel internal log back end and the internal `HTTPP_LOG(level)` macro (0091).

## Parts

### ADR-0011: Logging: AixLog behind `HTTPP_LOG`; request and bind-address logs in C++; defaults

- **Status:** accepted (the defaults were decided in this session; the owner left the method to me and accepted `run`)
- **Date:** 2026-10-18
- **Source:** `src/core/log.cpp`, `src/core/async_server.cpp`, `src/core/server.cpp`, `CHANGELOG.md`

#### Context

`python app.py` printed nothing: neither the bound address nor the requests. The library's default log level is `warning`, and
the `starting on` message that `run` printed came before the bind and also showed when the bind failed.

#### Decision

- Internal logging stays AixLog behind `HTTPP_LOG` (only `log.cpp` includes its header); the request log goes through the same path,
  so `set_log_level`, `set_log_file` and `set_log_callback` apply to it.
- One `info` line per response in `conn::send` (the point every response goes through, including the 404 and 413 the server makes itself):
  `GET /items/1 200`. The method and target are saved before the `std::move` and only when the `info` level is on.
- After a **successful bind**, `listening on http://host:port` (`info`), in both `listen` and `bind_to_any_port` (with the real port).
- `httpp.run` / `app.run()` / `httpp run` default to `info` (like uvicorn); the old `starting on` message was removed.
  `app.listen()` and the C++ API keep the default `warning`.
- The native commands `httpp server` and `httpp browse` also default to `info` (the address, then one line per request) and take `--log-level`
  with the same values as `httpp py run`. They call `listen()`, which stays at `warning` for the library, so without this they ran silent: the
  commands raise the level themselves (`set_log_level`) and nothing else changes. Added after the owner ran `httpp browse` and saw only the
  "Browsing ..." line.

#### Rejected alternatives

- **A Python `after_request` hook for logging:** Python and GIL cost on every request; used only as a temporary workaround in the example.
- **`info` as the default for the whole library:** an embedded server must not write to stderr without being asked, and it changes benchmarks.
  If `listen()` should also be chatty, this default has to change.

#### Consequences

- Every `info` line costs something under load (a lock and a write to stderr); benchmarks must run with `log_level="warning"` or `listen()`.
- If the method is not known before parsing finishes (a 400 error), the log line starts with an extra space.
- Tests: 3 C++ tests in `test_log.cpp` (on Linux and macOS in CI), `test_log.py` (in every wheel) and the CLI test in
  `test_run.py`, which is skipped on Windows because `SIGTERM` there is a hard kill ([ADR-0015](0015-testing-and-ci.md)).

#### Related

ADR-0003, ADR-0012

### ADR-0018: The C++ log goes through Python's `logging` by default, and the logger controls the C++ level

- **Status:** accepted
- **Date:** 2026-10-19
- **Decided by:** the project owner asked for it to be automatic; the design is mine
- **Source:** `src/bindings/python/httpp/_log.py`, `__init__.py`, `_run.py`, `tests/python/test_log_bridge.py`

#### Context

The C++ log (AixLog behind `HTTPP_LOG`, [ADR-0011](0011-logging.md)) had its own level, file and callback. A Python application
already configures logging with `logging`, and bridging the two needed a hand-written callback. The owner wanted this automatic.

#### Decision

- Importing `httpp` installs a callback that sends every C++ line to `logging.getLogger("httpp")` (Python-only code, ADR-0017 rule 4).
- **The logger decides the C++ level.** The C++ level follows `logger.getEffectiveLevel()` (`trace` below DEBUG, `debug`, `info`, `warning`,
  `error`, and `off` for CRITICAL and above). C++ builds a line only when its level lets it through, and a line that is sent costs a GIL
  acquisition, so the C++ level must not stay at `trace` just to let Python filter.
- `Logger.setLevel` of the `"httpp"` logger is wrapped to resync the C++ level. This covers `setLevel`, `caplog.set_level` and
  `logging.config.dictConfig`. A change that the logger only inherits from the root is not announced by `logging`, so
  `Server.listen` (and `run`) resync when a server starts.
- `httpp.set_log_level("info")` now sets the level of the `"httpp"` logger (so `logging` stays the one source of truth);
  an unknown name raises `ValueError`; `"off"` sets a level above CRITICAL.
- `httpp run` / `app.run()`: if the application configured no logging at all (no handler on the root or the `"httpp"` logger), a stderr handler
  with the format `%(asctime)s [%(levelname)s] (httpp) %(message)s` is added, so the output is still there (like uvicorn).
- An explicit `set_log_callback(...)` or `set_log_file(...)` replaces the bridge, because the C++ side has one sink at a time.
  `set_log_callback(None)` goes back to stderr from C++.
- C++ users are unchanged: the default level is `warning` and the sink is stderr.

#### Rejected alternatives

- **A `httpp.use_logging()` function the user has to call:** not automatic.
- **C++ level fixed at `trace`, Python filters:** every request would build a line and take the GIL even when nobody wants it.
- **A Python `Handler`/`Filter` that tries to see level changes:** `logging` has no hook for a level change; wrapping `setLevel` is the smallest thing that works.

#### Consequences

- With no logging configured and plain `app.listen()`, warnings and errors still appear (Python's last-resort handler prints WARNING and above,
  with only the message text); info lines need `logging.getLogger("httpp").setLevel(logging.INFO)` and a handler, or `app.run()`.
- `logging.config.dictConfig` with the default `disable_existing_loggers=True` disables the `"httpp"` logger unless it is mentioned,
  which silences the C++ log. Use `disable_existing_loggers: False`. (The Python docs describe this behavior of `dictConfig`.)
- Because `httpp.set_log_level` is now a Python function, a global `logging.disable(...)` or `Logger.disabled` also silences C++ output.
- The callback runs on the library's threads with the GIL taken and must not call back into httpp.
- The log level stays per process, not per `Server`.
- Tested on Linux: 18 tests in `test_log_bridge.py` (default bridge, level control, `caplog`, `dictConfig`, level mapping, explicit callback,
  disabled logger) and the CLI tests in `test_run.py`.

#### Related

ADR-0011, ADR-0017

### ADR-0050: An internal logging module, ported from libstb, replaces AixLog

- **Status:** accepted (supersedes the logging back end chosen in ADR-0003); the module was made public by ADR-0057
- **Source:** `src/core/internal/logging.hpp`, `src/core/logging.cpp`, `src/core/log.cpp`, `tests/cpp/test_log.cpp`

#### Context

httpp wrapped a vendored single-header AixLog (v1.5.0) carrying one patch (upstream redirects `std::clog` for the whole process), reached only through a private ostream, and used
about 10 percent of it: a level, a file, a callback and stderr. libstb, the same owner's library, already has a small logging module shaped like Python's `logging`, with no
dependency, thread-safe handlers, a formatter and a last-resort stderr output.

#### Decision

The module is copied into httpp as an internal (not installed, not exported) header and source under `httpp::logging`. Changes from libstb: namespace, a `trace` level (5), a `where`
field on the record (`%(funcName)s`) and `logger::log_from` for a message that is already built, the `stb::` helpers (`sformat`) copied in, `std::runtime_error` instead of `stb::io_error`,
and no `LIBSTB_LOG` environment variable. `src/core/log.cpp` keeps the public API of `httpp/log.hpp` (C++, Cython and Python unchanged) on top of one non-propagating logger named
`httpp`: `set_log_level` is one atomic (a disabled level still builds no message), `set_log_file` and `set_log_callback` swap the handler under a mutex. The vendored AixLog, its
patch and its include path are gone. Nothing of the module is public, so the dual-API rule is untouched; making it public later would need Python bindings.

#### Amendment 2026-11-10

`file_handler` takes a UTF-8 `std::string` (opened through `httpp::path::native`, ADR-0054, 0058), not `std::filesystem::path`: the macOS wheels target 10.13 and the macOS CI build failed on it. libstb's core logging files have not changed since the port
(2026-11-05); what libstb added since is the operating-system handlers (`logging_os`) and the Python bridge hardening, of which httpp took the shutdown part.

#### Consequences

About 800 lines of our own code replace a 3000-line vendored header and a patch to carry on every upgrade. The default line changes to `time [LEVEL] httpp (function): message`.
The two libraries now hold copies of the same module; if it is ever extracted into a shared package, both switch to it.

### ADR-0056: The operating system log handlers of libstb, behind `set_log_system`

- **Status:** accepted
- **Decided by:** the project owner ("yes, port them"), after the comparison with libstb's git log
- **Source:** `src/core/internal/logging_os.hpp`, `src/core/logging_os.cpp`, `include/httpp/log.hpp`, `tests/cpp/test_logging_os.cpp`, `tests/python/test_log.py`

#### Context

ADR-0050 ported libstb's logging module but left out its handlers for the log of the operating system (libstb added them later). A server run as a service wants its lines in journalctl, logcat, Console.app or Event Viewer.

#### Decision

- The five handlers are ported as they are (same classes, same level mapping: five severities, a level in between takes the one below): `syslog_handler`, `android_handler`, `unified_log_handler`, `debug_string_handler`, `event_log_handler`. They live in the internal logging module (`internal/logging_os.hpp`, `logging_os.cpp`), each only on the platform that has its facility (`HTTPP_LOG_HAS_*`). The one libstb helper that was a separate header (`logging_os_rank.hpp`) is folded into that header, and `stb::io_error` became `std::runtime_error`.
- One public function, the same in both languages, like `set_log_file`: C++ `set_log_system(log_system which = native, const std::string& name = "httpp")`, Python `set_log_system(which="native", name="httpp")` with `which` one of `"native"`, `"syslog"`, `"android"`, `"unified"`, `"debug_string"`, `"event_log"`. `native` is android on Android, unified on Apple, debug_string on Windows and syslog on the other Unix systems. It replaces the current output (stderr, a file, a callback) like the other setters.
- A system the platform does not have, or one that cannot be opened, returns false in C++ (the previous output is kept) and raises `OSError` in Python; an unknown name is a `ValueError`.
- The system adds time, level and process, so the line carries only `funcName: message` (debug_string, which shows no level, keeps `[LEVEL]`).
- syslog has one connection per process: its old handler is removed before the new `openlog()`, so a second call does not end up with `closelog()` after it.
- The handler classes stay internal: no public handler API. If someone needs to combine outputs (a file and syslog at once) that is a separate decision.

#### Consequences

- Only syslog could be run here (Linux): the tests feed every level to it and check the switching and the refusals. What journald, logcat, the unified log, the debugger output and the Event Log do with a record cannot be read back by a test, and the Android, Apple and Windows code was not compiled on those systems. CI is the first test of it.
- Windows links `advapi32`, Android links `log` (CMake).

#### Related

ADR-0050, ADR-0054

### ADR-0057: The logging module is public: `httpp/logging.hpp`

- **Status:** accepted; amends ADR-0050 (which kept the module internal)
- **Decided by:** the project owner ("ADRs can be wrong; yes, export it")
- **Source:** `include/httpp/logging.hpp`, `src/core/logging.cpp`, `tests/cpp/test_logging_module.cpp` (ported from libstb's tests)

#### Context

ADR-0050 ported libstb's logging module as an internal header, on three grounds: the dual-API rule (a public C++ type needs a Python counterpart), the cost of an ABI for derivable classes, and the four setters being enough. The owner disagreed: a C++ program that uses httpp should be able to log through the same module (its own loggers, a file and syslog at once, its own handler class), as it can with libstb.

#### Decision

- `src/core/internal/logging.hpp` moved to the installed header `include/httpp/logging.hpp`. Exported: `level_name`, `level_from_string`, `formatter`, `handler` (derivable: your own class overrides `emit()`), `null_handler`, `stream_handler`, `file_handler`, `callback_handler`, `logger`, `get_logger`, `basic_config`; `sformat` and friends are inline.
- The classes are marked `HTTPP_API` as a whole, not member by member (the convention of `export.hpp` for everything else): a class that callers derive from needs its vtable and type information exported, or the link fails on Linux. The header silences MSVC's C4251 for them (the private pimpl members are never touched by a caller).
- `file_handler` takes an `httpp::path` (ADR-0055), so a `std::string` still works, and opens its stream with `path::open`. No pimpl for the handlers: they hold their `std::fstream` / `std::function` directly, like the rest of the public API holds `std::string`s.
- The logger httpp itself logs to stays the non-propagating "httpp" logger, driven by `set_log_level`, `set_log_file`, `set_log_system` and `set_log_callback`: a program can add handlers to it, `get_logger("httpp").add_handler(...)`, like to any other.
- The operating system handlers of ADR-0056 stay internal (reached through `set_log_system`); making them public is a one-line move when someone asks.
- Python: no binding. Python's own `logging` is the module this one imitates, so the parity test maps `logging.hpp` to `None`. The "httpp" logger is already bridged into `logging.getLogger("httpp")` (`httpp/_log.py`). This is a deliberate exception to the dual-API rule, not a gap.

#### Consequences

- The module is now API: changing `handler`, `logger` or the record layout changes the ABI.
- The test program no longer compiles `logging.cpp` itself; it uses the exported one, which exercises the export on every platform CI has.
- On Windows the export of the whole classes was not compiled by anyone yet.

#### Related

ADR-0050, ADR-0054, ADR-0055, ADR-0056

### ADR-0075: one logging header and one logging source

- **Status:** accepted; its `log_level` enum is superseded by ADR-0085 (`httpp::logging::level` is the one level type)
- **Decided by:** the project owner (several files named log*; "logging.hpp and .cpp at most", logging as the standard name); the choice to keep `set_log_*` is mine
- **Source:** `include/httpp/logging.hpp`, `src/core/logging.cpp`, `src/core/internal/logging.hpp`, `tests/cpp/test_logging_module.cpp`

#### Context

The logging code lived in `log.hpp`/`log.cpp` (the library's own log: `set_log_level`, `set_log_file`, `set_log_system`, `set_log_callback`, a thin face over the `"httpp"` logger) beside `logging.hpp`/`logging.cpp` (the module, ADR-0050/0057), an internal `log.hpp` with the `HTTPP_LOG` macro, and `test_log.cpp` beside `test_logging_module.cpp`. Two names for one subject.

#### Decision

- `httpp/log.hpp` is removed; its declarations are at the end of `httpp/logging.hpp` (same names, same `httpp` namespace). `log.cpp` is now the last part of `logging.cpp`; `src/core/internal/log.hpp` became `internal/logging.hpp`; `test_log.cpp` is part of `test_logging_module.cpp`.
- The `set_log_*` functions are kept, not retired: Python's `httpp.set_log_level` and friends, the CLI and about 180 uses depend on them, and a C++ program would otherwise repeat the handler setup behind each of them (and the OS handlers are internal, ADR-0056).
- The operating system handlers of ADR-0056 (`syslog_handler`, `android_handler`, `unified_log_handler`, `debug_string_handler`, `event_log_handler`) are public and live in `logging.hpp`/`logging.cpp` too (`internal/logging_os.hpp` and `logging_os.cpp` are gone), so the whole of logging is one header and one source. The system headers and their macros stay inside `logging.cpp`, which is one translation unit: the `HTTPP_LOG_HAS_*` platform macros are plain `#if`s in the header, and on Windows the source defines `WIN32_LEAN_AND_MEAN`, `NOMINMAX` and `NOGDI` before `<windows.h>` and `#undef`s `ERROR`, `min` and `max` after it. (The first draft of this ADR kept them apart for the macros; that reason only held for a header.)
- Parity test: `logging.hpp` now maps to `httpp`; the four module functions are listed in `NOT_EXPOSED` (Python's own `logging` is their counterpart).

#### Consequences

- **Breaking for C++ code (major is 0):** `#include "httpp/log.hpp"` no longer exists; include `httpp/logging.hpp`. Nothing else changes.
- Fewer files; `logging.cpp` is about 1000 lines. The handler classes are `HTTPP_API`, so they can be derived from or used by C++ programs (ADR-0057 said this was a one-line move).
- Python is unchanged: the "httpp" logger is bridged both ways with the standard `logging` module (`httpp/_log.py`, ADR-0018): C++ lines reach Python handlers, and `logging.getLogger("httpp").setLevel(...)`, `dictConfig` or `caplog.set_level` set the C++ level.

#### Related

ADR-0050, ADR-0056, ADR-0057

### ADR-0080: `httpp.bridge_logger(name)` sends any C++ logger tree into Python's `logging`

- **Status:** accepted
- **Date:** 2026-11-17
- **Decided by:** the project owner (libspeech should not keep a Python log bridge of its own; "pass everything to httpp"); the design is mine
- **Source:** `src/bindings/python/cy/support.pxi` (`_bridge_logger`, `_unbridge_logger`, `_set_logger_level`, `_log_from_cpp`), `src/bindings/python/httpp/_log.py` (`bridge_logger`), `tests/python/test_log_bridge_any_logger.py`
- **Builds on:** ADR-0018 (the `"httpp"` bridge), ADR-0057 (the public `httpp::logging` module), ADR-0074 (`callback_handler` without the lock)

#### Context

`httpp::logging` (ADR-0057) is a public, Python-like logger tree, and libspeech is the first program that uses it with loggers of its own
(`libspeech.dsp.stft`, `libspeech.models.files`, ...). Its Python side needs those lines in `logging`, with the same names and with the
Python levels deciding what C++ produces. The bridge that exists (ADR-0018) cannot do it:

- `httpp::set_log_callback`, `set_log_level` and `set_log_file` work on the one logger named `"httpp"` (`propagate = false`, its own handlers);
  records of a logger outside that tree reach no handler.
- `_log.py` hands every line to `logging.getLogger("httpp")` and drops the logger's name.
- Only the `"httpp"` logger's `setLevel` is wrapped, so a level set on a logger below it, or by `dictConfig` for such a logger, never reaches C++.

Without a general bridge each consumer copies `_log.py` and the Cython callback (libspeech had both, about 190 lines and a test file of the same size).

#### Decision

- `httpp.bridge_logger(name)`: the C++ logger `name` gets one `callback_handler` (**`serialized = false`**, ADR-0074) and `propagate = false`;
  Python's `logging` does the propagating. The callback takes the GIL and calls `_emit_record(levelno, logger_name, where, message)`, which builds a
  `LogRecord` for `logging.getLogger(logger_name)` with `where` as `funcName` and hands it to that logger's handlers.
- **Levels are mirrored per logger.** A C++ logger below the root gets the `level` of its Python logger (0 inherits, as in Python). The bridged root
  gets its **effective** level, so `logging.basicConfig(level=DEBUG)` on the application's root logger counts when the bridged logger only inherits.
- **`logging.Logger.setLevel` is wrapped once, for every logger** (not only one instance as in ADR-0018), the first time a bridge is made. That is what makes `getLogger("libspeech.dsp").setLevel(...)`,
  `dictConfig` and `basicConfig` work for loggers nobody knows in advance. After the original `setLevel` runs, the C++ levels of the affected bridged
  names are synchronized. Levels that were already set when the bridge is made are copied then.
- Refused with `ValueError`: an empty name or `"root"`, `"httpp"` and the names below it (bridged by httpp itself), and a name that contains or is
  inside one already bridged (a record would be delivered twice). Calling it again for the same name is fine.
- An `atexit` hook unbridges every name (the handlers go away), and the trampoline returns at once when `Py_IsInitialized()` is false: the same two rules as ADR-0018's `_unhook` / `_log_trampoline`.
- `_log_from_cpp(name, level, where, message, count, threads)` is a **test seam** (underscore, not exported): it logs from the calling thread with the GIL held, and from library threads, so the
  deadlock case of ADR-0074 is tested through a real Python binding.

#### Rejected alternatives

- **Generalize the `"httpp"` bridge (`set_log_callback`) to a tree.** It is a one-sink facade by design (ADR-0018, ADR-0050); changing what `set_log_callback(None)` or `set_log_file` mean for the `"httpp"` logger is a different decision.
- **Wrap `setLevel` of every logger below the root only (instance by instance).** Loggers are created by `getLogger` at any time; no hook sees them.
- **C++ level fixed at `debug`, Python filters.** Every line would be built and would take the GIL even when nobody wants it (the argument of ADR-0018).
- **Each consumer keeps its own `_log.py`.** The code is the same everywhere and the deadlock rule is easy to get wrong.

#### Consequences

- A C++ library that logs through `httpp::logging` needs one line in its `__init__.py`: `httpp.bridge_logger("libspeech")`. It needs no Python callback, no `set_log_*` and no `_log.py` of its own.
- `logging.Logger.setLevel` is replaced (by a function that calls the original) in every process that makes a bridge. A level set while no bridge exists is picked up when the bridge is made.
- **Not verified:** Windows and macOS; Python 3.13 and free-threaded builds. A library thread that logs in the middle of interpreter finalization can still reach `PyGILState_Ensure` between the `Py_IsInitialized()` check and the call (the window is the same as ADR-0018's); in an experiment a thread that was still logging when the interpreter exited aborted the process, so a library that owns such threads should stop them in its own `atexit` hook.

### ADR-0085: one log level type, `httpp::logging::level`

- **Status:** accepted; supersedes the `log_level` enum of ADR-0075
- **Decided by:** the project owner ("`httpp::logging::level` is good")
- **Source:** `include/httpp/logging.hpp`, `src/core/logging.cpp`, `src/core/internal/logging.hpp`, `src/bindings/python/cy/support.pxi`, `tests/cpp/test_logging_module.cpp`, `tests/cpp/test_server_lifecycle.cpp`

#### Context

The header had two enums for the same idea. `httpp::logging::level` (Python's numbers: notset, trace 5, debug 10, info 20, warning 30, error 40, critical 50) was the level of a record and of a logger in the logger tree, with `level_from_string`. `httpp::log_level` (trace, debug, info, warning, error, off) was the level of the library's own log, with `set_log_level`. They did not convert into each other, did not match (`critical` and `notset` on one, `off` on the other) and had two tables of names; ADR-0078 added `set_log_level(name)` with a second table instead of joining them.

#### Decision

- **`httpp::logging::level` is the only level type.** `log_level` is removed.
- **`level::off = 60`** is new: above critical, a threshold only. On a logger, or in `set_log_level`, it lets nothing through; no record is ever made at it. `level_name(off)` is `"OFF"`, `level_from_string("off")` gives it.
- **`set_log_level(logging::level)`** and **`set_log_callback(fn(logging::level, where, message))`** take and give that type. The callback gets the level of the record as it was made; before, it was rounded to one of five values.
- **`set_log_level(name)` uses `level_from_string`**: the one table of names, so it also knows `warn`, `fatal`, `critical`, `notset` and a number, in any case. A name it does not know returns false and changes nothing.
- The `httpp` commands (`--log-level`) and Python (`httpp.set_log_level("info")`) still take the same six words; Python passes the name to C++ and no longer has a copy of the enum.

#### Rejected alternatives

- **Keeping `log_level` as an alias of `logging::level`.** Two names for one type is the confusion this removes, and the alias would keep old code compiling for a reason nobody has to know.
- **Merging into `log_level` instead.** It has no `critical`, no `notset` and no room between the levels, which the logger tree needs.
- **A conversion function between the two enums.** It cannot be one-to-one (`off`, `critical`, `notset`).

#### Consequences

- Breaking for C++ code that wrote `httpp::log_level::debug` or a callback taking `httpp::log_level`: write `httpp::logging::level::debug`. Python is unchanged.
- `set_log_level("notset")` or a number is now accepted; it is a threshold like any other (`notset` = 0 lets everything through, as `trace` does).
- The library's own log is still one logger named `httpp`; only its level type changed.

#### Related

ADR-0075 (one logging header), ADR-0078 (the commands call the library), ADR-0011 (logging).

### ADR-0090: stream-style logging and an inline level check (`HTTPP_LOG`, `HTTPP_LOGF`, `logger::enabled`)

- **Status:** accepted
- **Decided by:** the project owner (both styles side by side, `sformat` stays, Python unchanged); the macro names, the cache design and the ABI bump are mine
- **Source:** `include/httpp/logging.hpp`, `src/utils/logging.cpp`, `tests/utils/fast_path.cpp`

#### Context

`log.debug("x=%d", compute())` builds the text only when the level is enabled, but `compute()` always runs (C++ evaluates the arguments first), and the call itself crosses into `libhttpp_utils`, where `is_enabled_for` walks up the logger tree. For a library that logs from inner loops (libspeech's DSP) a disabled line should cost almost nothing. C++ programs also want `<<` for types that have it (`std::filesystem::path`, `std::chrono`, their own) without `.str()` and `%s`.

#### Decision

- **Two styles, both kept.** The printf-style methods (`debug`, `info`, ..., `log`) and `sformat` are unchanged. Two macros are added; each skips its whole argument list when the level is disabled:
  - `HTTPP_LOG(log, info) << "x=" << x << path;` (streams)
  - `HTTPP_LOGF(log, info, "x=%d", x);` (printf style, checked by the compiler)
  They can be mixed (`HTTPP_LOG(log, info) << sformat("%05.1f", v) << " ms"`). The level is a bare name (`trace` ... `critical`). `log` is evaluated once. Each macro is one statement (no dangling else). The library's own internal macro, which used the name `HTTPP_LOG(level)`, is renamed `HTTPP_LIB_LOG(level)` (src/utils/internal/logging.hpp, not installed) so the public names are `HTTPP_LOG` and `HTTPP_LOGF`.
- **`logger::enabled(level)` is inline.** Each logger holds a cached effective level next to a process-wide epoch (`detail::level_epoch`, exported, constant-initialized). `set_level()` on any logger bumps the epoch. `enabled()` reads the cache and the epoch (two relaxed loads, one compare); when the epoch moved it calls `enabled_slow()`, which recomputes the effective level (the walk up the tree) and stores it. The epoch is read before the level, so a concurrent `set_level()` can only make the cache look stale, never fresh. A line may be logged or skipped once more after another thread changed a level, as with the relaxed atomics before.
- **Stream buffer.** The text goes to a `std::ostringstream` reused per thread (flags, width and fill are reset for every line); a line started while another of the same thread is being built (an `operator<<` that logs) gets its own stream. The proxy never throws; a failing `operator<<` loses that line. Only the finished text crosses into the library (`logger::log_from`).
- **ABI.** The `logger` object grew by one atomic, so `HTTPP_UTILS_ABI_VERSION` is 2 (ADR-0086's check catches a mismatch at load).
- **Python.** Unchanged. `logging.Logger.info("x=%s", x)` and f-strings are Python's own; the bridge (ADR-0080) receives the finished text.

#### Rejected alternatives

- **A process-wide minimum level as the only check.** One load, but it only gets less precise after any logger was ever lowered (a raise cannot be recomputed cheaply), so one `debug` logger would slow down every other.
- **`log.info() << ...` as a method.** The arguments are always evaluated, which is the cost this ADR removes. It may be added as sugar later, with that warning.
- **A new `std::ostringstream` per line.** Constructing it (locale, allocation) costs more than a disabled line should.

#### Consequences

- A disabled line costs two loads and a compare, with no call and no argument evaluation, when written with the macros. The method form `log.debug(...)` keeps its old cost.
- Memory: one `std::atomic<std::uint64_t>` per logger.
- The macros need a C++17 compiler (`if` with an initializer).
- Not measured: no benchmark was run; the cost above is from the code, not from a timing.

### ADR-0091: the library's own log is the logger `"httpp"`; no second level, no second macro system

- **Status:** accepted
- **Decided by:** the project owner (asked for the principled form: the internal macro looked like a leftover); the design is mine
- **Source:** `src/utils/logging.cpp`, `src/utils/internal/logging.hpp`, `include/httpp/logging.hpp`, `src/core/async_server.cpp`, `tests/cpp/test_logging_module.cpp`

#### Context

The library's own log (`httpp::set_log_level`, `set_log_file`, `set_log_callback`, `set_log_system`) was a thin face over the logger `"httpp"`, but it kept a parallel back end: an internal macro `HTTPP_LOG(level)` (born in the AixLog days to keep AixLog's global macros out of every file, ADR-0011), its own line buffer (`log_line`), its own threshold (`detail::own_log_level`, an atomic) and its own writer (`detail::log_write`). The logger `"httpp"` itself sat at `trace` and let the threshold decide. So the library's log level was not the level of the logger: `get_logger("httpp").set_level(...)` did nothing, and a child such as `"httpp.x"` would have inherited `trace`. Since ADR-0090 the public `HTTPP_LOG` and `HTTPP_LOGF` do what the internal macro did.

#### Decision

- **The level of the library's log is the level of the logger `"httpp"`.** `set_log_level(level)` calls `set_level` on it; the default is `warning`. `own_log_level`, `log_write`, `log_enabled` and `log_line` are removed.
- **One macro system.** The library logs with the public macro on its own logger: `HTTPP_LIB_LOG(level)` (internal header, not installed) is the one-line alias `HTTPP_LOG(lib_logger(), level)`. It has the same laziness (`logger::enabled`, two relaxed loads and a compare) and the same stream buffer as the public macro. `lib_logger()` is the public `get_logger("httpp")`, looked up once per module (the lookup takes a lock); nothing is exported for it, and the engine tests that compile core sources into the test binary get the same logger from the registry. The registry gives the logger `"httpp"` its defaults when it is created (`warning`, stderr, no propagation to the root), so whoever asks first finds it set up.
- **Consequences for users.** `get_logger("httpp").set_level(...)` and `set_log_level(...)` are now the same thing; a child logger `"httpp.x"` follows the library's level instead of being open at `trace`. The Python side is unchanged: `httpp.set_log_level` and `logging.getLogger("httpp").setLevel` still end in `set_log_level` through `sync()`.
- **ABI.** `own_log_level`, `log_write` and a short-lived exported function `lib_logger` were exported symbols of `libhttpp_utils` (the name now belongs to the inline, not exported `detail::lib_logger()`), so `HTTPP_UTILS_ABI_VERSION` is 4.

#### Rejected alternatives

- **Keep the parallel back end** and only rename the macro: two systems for one job, and a level that the logger tree does not see.
- **Remove `HTTPP_LIB_LOG` and write `HTTPP_LOG(lib_logger(), warning)` at the 38 call sites:** the alias is one line and keeps the sites short; it adds no behavior of its own.

#### Consequences

- Fewer exported symbols, one place where a level is decided.
- Not verified here: the Python test suite (`test_log.py`, `test_log_bridge*.py`) was not run; the C++ suite was.
