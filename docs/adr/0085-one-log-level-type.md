# ADR-0085: one log level type, `httpp::logging::level`

- **Status:** accepted; supersedes the `log_level` enum of ADR-0075
- **Decided by:** the project owner ("`httpp::logging::level` is good")
- **Source:** `include/httpp/logging.hpp`, `src/core/logging.cpp`, `src/core/internal/logging.hpp`, `src/bindings/python/cy/support.pxi`, `tests/cpp/test_logging_module.cpp`, `tests/cpp/test_server_lifecycle.cpp`

## Context

The header had two enums for the same idea. `httpp::logging::level` (Python's numbers: notset, trace 5, debug 10, info 20, warning 30, error 40, critical 50) was the level of a record and of a logger in the logger tree, with `level_from_string`. `httpp::log_level` (trace, debug, info, warning, error, off) was the level of the library's own log, with `set_log_level`. They did not convert into each other, did not match (`critical` and `notset` on one, `off` on the other) and had two tables of names; ADR-0078 added `set_log_level(name)` with a second table instead of joining them.

## Decision

- **`httpp::logging::level` is the only level type.** `log_level` is removed.
- **`level::off = 60`** is new: above critical, a threshold only. On a logger, or in `set_log_level`, it lets nothing through; no record is ever made at it. `level_name(off)` is `"OFF"`, `level_from_string("off")` gives it.
- **`set_log_level(logging::level)`** and **`set_log_callback(fn(logging::level, where, message))`** take and give that type. The callback gets the level of the record as it was made; before, it was rounded to one of five values.
- **`set_log_level(name)` uses `level_from_string`**: the one table of names, so it also knows `warn`, `fatal`, `critical`, `notset` and a number, in any case. A name it does not know returns false and changes nothing.
- The `httpp` commands (`--log-level`) and Python (`httpp.set_log_level("info")`) still take the same six words; Python passes the name to C++ and no longer has a copy of the enum.

## Rejected alternatives

- **Keeping `log_level` as an alias of `logging::level`.** Two names for one type is the confusion this removes, and the alias would keep old code compiling for a reason nobody has to know.
- **Merging into `log_level` instead.** It has no `critical`, no `notset` and no room between the levels, which the logger tree needs.
- **A conversion function between the two enums.** It cannot be one-to-one (`off`, `critical`, `notset`).

## Consequences

- Breaking for C++ code that wrote `httpp::log_level::debug` or a callback taking `httpp::log_level`: write `httpp::logging::level::debug`. Python is unchanged.
- `set_log_level("notset")` or a number is now accepted; it is a threshold like any other (`notset` = 0 lets everything through, as `trace` does).
- The library's own log is still one logger named `httpp`; only its level type changed.

## Related

ADR-0075 (one logging header), ADR-0078 (the commands call the library), ADR-0011 (logging).
