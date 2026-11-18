# ADR-0091: the library's own log is the logger `"httpp"`; no second level, no second macro system

- **Status:** accepted
- **Decided by:** the project owner (asked for the principled form: the internal macro looked like a leftover); the design is mine
- **Source:** `src/utils/logging.cpp`, `src/utils/internal/logging.hpp`, `include/httpp/logging.hpp`, `src/core/async_server.cpp`, `tests/cpp/test_logging_module.cpp`

## Context

The library's own log (`httpp::set_log_level`, `set_log_file`, `set_log_callback`, `set_log_system`) was a thin face over the logger `"httpp"`, but it kept a parallel back end: an internal macro `HTTPP_LOG(level)` (born in the AixLog days to keep AixLog's global macros out of every file, ADR-0011), its own line buffer (`log_line`), its own threshold (`detail::own_log_level`, an atomic) and its own writer (`detail::log_write`). The logger `"httpp"` itself sat at `trace` and let the threshold decide. So the library's log level was not the level of the logger: `get_logger("httpp").set_level(...)` did nothing, and a child such as `"httpp.x"` would have inherited `trace`. Since ADR-0090 the public `HTTPP_LOG` and `HTTPP_LOGF` do what the internal macro did.

## Decision

- **The level of the library's log is the level of the logger `"httpp"`.** `set_log_level(level)` calls `set_level` on it; the default is `warning`. `own_log_level`, `log_write`, `log_enabled` and `log_line` are removed.
- **One macro system.** The library logs with the public macro on its own logger: `HTTPP_LIB_LOG(level)` (internal header, not installed) is the one-line alias `HTTPP_LOG(lib_log(), level)`. It has the same laziness (`logger::enabled`, two relaxed loads and a compare) and the same stream buffer as the public macro. `lib_log()` looks the logger up once per module; `lib_logger()` is exported so the engine tests, which compile core sources into the test binary, share the library's logger.
- **Consequences for users.** `get_logger("httpp").set_level(...)` and `set_log_level(...)` are now the same thing; a child logger `"httpp.x"` follows the library's level instead of being open at `trace`. The Python side is unchanged: `httpp.set_log_level` and `logging.getLogger("httpp").setLevel` still end in `set_log_level` through `sync()`.
- **ABI.** `own_log_level` and `log_write` were exported symbols of `libhttpp_utils`, so `HTTPP_UTILS_ABI_VERSION` is 3.

## Rejected alternatives

- **Keep the parallel back end** and only rename the macro: two systems for one job, and a level that the logger tree does not see.
- **Remove `HTTPP_LIB_LOG` and write `HTTPP_LOG(lib_log(), warning)` at the 38 call sites:** the alias is one line and keeps the sites short; it adds no behavior of its own.

## Consequences

- Fewer exported symbols, one place where a level is decided.
- Not verified here: the Python test suite (`test_log.py`, `test_log_bridge*.py`) was not run; the C++ suite was.
