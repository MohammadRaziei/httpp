# ADR-0018: The C++ log goes through Python's `logging` by default, and the logger controls the C++ level

- **Status:** accepted
- **Date:** 2026-10-19
- **Decided by:** the project owner asked for it to be automatic; the design is mine
- **Source:** `src/bindings/python/httpp/_log.py`, `__init__.py`, `_run.py`, `tests/python/test_log_bridge.py`

## Context

The C++ log (AixLog behind `HTTPP_LOG`, [ADR-0011](0011-logging.md)) had its own level, file and callback. A Python application
already configures logging with `logging`, and bridging the two needed a hand-written callback. The owner wanted this automatic.

## Decision

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

## Rejected alternatives

- **A `httpp.use_logging()` function the user has to call:** not automatic.
- **C++ level fixed at `trace`, Python filters:** every request would build a line and take the GIL even when nobody wants it.
- **A Python `Handler`/`Filter` that tries to see level changes:** `logging` has no hook for a level change; wrapping `setLevel` is the smallest thing that works.

## Consequences

- With no logging configured and plain `app.listen()`, warnings and errors still appear (Python's last-resort handler prints WARNING and above,
  with only the message text); info lines need `logging.getLogger("httpp").setLevel(logging.INFO)` and a handler, or `app.run()`.
- `logging.config.dictConfig` with the default `disable_existing_loggers=True` disables the `"httpp"` logger unless it is mentioned,
  which silences the C++ log. Use `disable_existing_loggers: False`. (The Python docs describe this behavior of `dictConfig`.)
- Because `httpp.set_log_level` is now a Python function, a global `logging.disable(...)` or `Logger.disabled` also silences C++ output.
- The callback runs on the library's threads with the GIL taken and must not call back into httpp.
- The log level stays per process, not per `Server`.
- Tested on Linux: 18 tests in `test_log_bridge.py` (default bridge, level control, `caplog`, `dictConfig`, level mapping, explicit callback,
  disabled logger) and the CLI tests in `test_run.py`.

## Related

ADR-0011, ADR-0017
