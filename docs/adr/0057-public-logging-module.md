# ADR-0057: The logging module is public: `httpp/logging.hpp`

- **Status:** accepted; amends ADR-0050 (which kept the module internal)
- **Decided by:** the project owner ("ADRs can be wrong; yes, export it")
- **Source:** `include/httpp/logging.hpp`, `src/core/logging.cpp`, `tests/cpp/test_logging_module.cpp` (ported from libstb's tests)

## Context

ADR-0050 ported libstb's logging module as an internal header, on three grounds: the dual-API rule (a public C++ type needs a Python counterpart), the cost of an ABI for derivable classes, and the four setters being enough. The owner disagreed: a C++ program that uses httpp should be able to log through the same module (its own loggers, a file and syslog at once, its own handler class), as it can with libstb.

## Decision

- `src/core/internal/logging.hpp` moved to the installed header `include/httpp/logging.hpp`. Exported: `level_name`, `level_from_string`, `formatter`, `handler` (derivable: your own class overrides `emit()`), `null_handler`, `stream_handler`, `file_handler`, `callback_handler`, `logger`, `get_logger`, `basic_config`; `sformat` and friends are inline.
- The classes are marked `HTTPP_API` as a whole, not member by member (the convention of `export.hpp` for everything else): a class that callers derive from needs its vtable and type information exported, or the link fails on Linux. The header silences MSVC's C4251 for them (the private pimpl members are never touched by a caller).
- `file_handler` takes an `httpp::path` (ADR-0055), so a `std::string` still works.
- The logger httpp itself logs to stays the non-propagating "httpp" logger, driven by `set_log_level`, `set_log_file`, `set_log_system` and `set_log_callback`: a program can add handlers to it, `get_logger("httpp").add_handler(...)`, like to any other.
- The operating system handlers of ADR-0056 stay internal (reached through `set_log_system`); making them public is a one-line move when someone asks.
- Python: no binding. Python's own `logging` is the module this one imitates, so the parity test maps `logging.hpp` to `None`. The "httpp" logger is already bridged into `logging.getLogger("httpp")` (`httpp/_log.py`). This is a deliberate exception to the dual-API rule, not a gap.

## Consequences

- The module is now API: changing `handler`, `logger` or the record layout changes the ABI.
- The test program no longer compiles `logging.cpp` itself; it uses the exported one, which exercises the export on every platform CI has.
- On Windows the export of the whole classes (std members in `file_handler`) was not compiled by anyone yet.

## Related

ADR-0050, ADR-0054, ADR-0055, ADR-0056
