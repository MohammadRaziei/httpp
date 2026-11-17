# ADR-0075: one logging header and one logging source

- **Status:** accepted; its `log_level` enum is superseded by ADR-0085 (`httpp::logging::level` is the one level type)
- **Decided by:** the project owner (several files named log*; "logging.hpp and .cpp at most", logging as the standard name); the choice to keep `set_log_*` is mine
- **Source:** `include/httpp/logging.hpp`, `src/core/logging.cpp`, `src/core/internal/logging.hpp`, `tests/cpp/test_logging_module.cpp`

## Context

The logging code lived in `log.hpp`/`log.cpp` (the library's own log: `set_log_level`, `set_log_file`, `set_log_system`, `set_log_callback`, a thin face over the `"httpp"` logger) beside `logging.hpp`/`logging.cpp` (the module, ADR-0050/0057), an internal `log.hpp` with the `HTTPP_LOG` macro, and `test_log.cpp` beside `test_logging_module.cpp`. Two names for one subject.

## Decision

- `httpp/log.hpp` is removed; its declarations are at the end of `httpp/logging.hpp` (same names, same `httpp` namespace). `log.cpp` is now the last part of `logging.cpp`; `src/core/internal/log.hpp` became `internal/logging.hpp`; `test_log.cpp` is part of `test_logging_module.cpp`.
- The `set_log_*` functions are kept, not retired: Python's `httpp.set_log_level` and friends, the CLI and about 180 uses depend on them, and a C++ program would otherwise repeat the handler setup behind each of them (and the OS handlers are internal, ADR-0056).
- The operating system handlers of ADR-0056 (`syslog_handler`, `android_handler`, `unified_log_handler`, `debug_string_handler`, `event_log_handler`) are public and live in `logging.hpp`/`logging.cpp` too (`internal/logging_os.hpp` and `logging_os.cpp` are gone), so the whole of logging is one header and one source. The system headers and their macros stay inside `logging.cpp`, which is one translation unit: the `HTTPP_LOG_HAS_*` platform macros are plain `#if`s in the header, and on Windows the source defines `WIN32_LEAN_AND_MEAN`, `NOMINMAX` and `NOGDI` before `<windows.h>` and `#undef`s `ERROR`, `min` and `max` after it. (The first draft of this ADR kept them apart for the macros; that reason only held for a header.)
- Parity test: `logging.hpp` now maps to `httpp`; the four module functions are listed in `NOT_EXPOSED` (Python's own `logging` is their counterpart).

## Consequences

- **Breaking for C++ code (major is 0):** `#include "httpp/log.hpp"` no longer exists; include `httpp/logging.hpp`. Nothing else changes.
- Fewer files; `logging.cpp` is about 1000 lines. The handler classes are `HTTPP_API`, so they can be derived from or used by C++ programs (ADR-0057 said this was a one-line move).
- Python is unchanged: the "httpp" logger is bridged both ways with the standard `logging` module (`httpp/_log.py`, ADR-0018): C++ lines reach Python handlers, and `logging.getLogger("httpp").setLevel(...)`, `dictConfig` or `caplog.set_level` set the C++ level.

## Related

ADR-0050, ADR-0056, ADR-0057
