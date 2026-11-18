# ADR-0090: stream-style logging and an inline level check (`HTTPP_LOG`, `HTTPP_LOGF`, `logger::enabled`)

- **Status:** accepted
- **Decided by:** the project owner (both styles side by side, `sformat` stays, Python unchanged); the macro names, the cache design and the ABI bump are mine
- **Source:** `include/httpp/logging.hpp`, `src/utils/logging.cpp`, `tests/utils/fast_path.cpp`

## Context

`log.debug("x=%d", compute())` builds the text only when the level is enabled, but `compute()` always runs (C++ evaluates the arguments first), and the call itself crosses into `libhttpp_utils`, where `is_enabled_for` walks up the logger tree. For a library that logs from inner loops (libspeech's DSP) a disabled line should cost almost nothing. C++ programs also want `<<` for types that have it (`std::filesystem::path`, `std::chrono`, their own) without `.str()` and `%s`.

## Decision

- **Two styles, both kept.** The printf-style methods (`debug`, `info`, ..., `log`) and `sformat` are unchanged. Two macros are added; each skips its whole argument list when the level is disabled:
  - `HTTPP_LOG(log, info) << "x=" << x << path;` (streams)
  - `HTTPP_LOGF(log, info, "x=%d", x);` (printf style, checked by the compiler)
  They can be mixed (`HTTPP_LOG(log, info) << sformat("%05.1f", v) << " ms"`). The level is a bare name (`trace` ... `critical`). `log` is evaluated once. Each macro is one statement (no dangling else). The library's own internal macro, which used the name `HTTPP_LOG(level)`, is renamed `HTTPP_LIB_LOG(level)` (src/utils/internal/logging.hpp, not installed) so the public names are `HTTPP_LOG` and `HTTPP_LOGF`.
- **`logger::enabled(level)` is inline.** Each logger holds a cached effective level next to a process-wide epoch (`detail::level_epoch`, exported, constant-initialized). `set_level()` on any logger bumps the epoch. `enabled()` reads the cache and the epoch (two relaxed loads, one compare); when the epoch moved it calls `enabled_slow()`, which recomputes the effective level (the walk up the tree) and stores it. The epoch is read before the level, so a concurrent `set_level()` can only make the cache look stale, never fresh. A line may be logged or skipped once more after another thread changed a level, as with the relaxed atomics before.
- **Stream buffer.** The text goes to a `std::ostringstream` reused per thread (flags, width and fill are reset for every line); a line started while another of the same thread is being built (an `operator<<` that logs) gets its own stream. The proxy never throws; a failing `operator<<` loses that line. Only the finished text crosses into the library (`logger::log_from`).
- **ABI.** The `logger` object grew by one atomic, so `HTTPP_UTILS_ABI_VERSION` is 2 (ADR-0086's check catches a mismatch at load).
- **Python.** Unchanged. `logging.Logger.info("x=%s", x)` and f-strings are Python's own; the bridge (ADR-0080) receives the finished text.

## Rejected alternatives

- **A process-wide minimum level as the only check.** One load, but it only gets less precise after any logger was ever lowered (a raise cannot be recomputed cheaply), so one `debug` logger would slow down every other.
- **`log.info() << ...` as a method.** The arguments are always evaluated, which is the cost this ADR removes. It may be added as sugar later, with that warning.
- **A new `std::ostringstream` per line.** Constructing it (locale, allocation) costs more than a disabled line should.

## Consequences

- A disabled line costs two loads and a compare, with no call and no argument evaluation, when written with the macros. The method form `log.debug(...)` keeps its old cost.
- Memory: one `std::atomic<std::uint64_t>` per logger.
- The macros need a C++17 compiler (`if` with an initializer).
- Not measured: no benchmark was run; the cost above is from the code, not from a timing.
