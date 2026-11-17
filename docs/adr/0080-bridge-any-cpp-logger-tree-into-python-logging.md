# ADR-0080: `httpp.bridge_logger(name)` sends any C++ logger tree into Python's `logging`

- **Status:** accepted
- **Date:** 2026-11-17
- **Decided by:** the project owner (libspeech should not keep a Python log bridge of its own; "pass everything to httpp"); the design is mine
- **Source:** `src/bindings/python/cy/support.pxi` (`_bridge_logger`, `_unbridge_logger`, `_set_logger_level`, `_log_from_cpp`), `src/bindings/python/httpp/_log.py` (`bridge_logger`), `tests/python/test_log_bridge_any_logger.py`
- **Builds on:** ADR-0018 (the `"httpp"` bridge), ADR-0057 (the public `httpp::logging` module), ADR-0074 (`callback_handler` without the lock)

## Context

`httpp::logging` (ADR-0057) is a public, Python-like logger tree, and libspeech is the first program that uses it with loggers of its own
(`libspeech.dsp.stft`, `libspeech.models.files`, ...). Its Python side needs those lines in `logging`, with the same names and with the
Python levels deciding what C++ produces. The bridge that exists (ADR-0018) cannot do it:

- `httpp::set_log_callback`, `set_log_level` and `set_log_file` work on the one logger named `"httpp"` (`propagate = false`, its own handlers);
  records of a logger outside that tree reach no handler.
- `_log.py` hands every line to `logging.getLogger("httpp")` and drops the logger's name.
- Only the `"httpp"` logger's `setLevel` is wrapped, so a level set on a logger below it, or by `dictConfig` for such a logger, never reaches C++.

Without a general bridge each consumer copies `_log.py` and the Cython callback (libspeech had both, about 190 lines and a test file of the same size).

## Decision

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

## Rejected alternatives

- **Generalize the `"httpp"` bridge (`set_log_callback`) to a tree.** It is a one-sink facade by design (ADR-0018, ADR-0050); changing what `set_log_callback(None)` or `set_log_file` mean for the `"httpp"` logger is a different decision.
- **Wrap `setLevel` of every logger below the root only (instance by instance).** Loggers are created by `getLogger` at any time; no hook sees them.
- **C++ level fixed at `debug`, Python filters.** Every line would be built and would take the GIL even when nobody wants it (the argument of ADR-0018).
- **Each consumer keeps its own `_log.py`.** The code is the same everywhere and the deadlock rule is easy to get wrong.

## Consequences

- A C++ library that logs through `httpp::logging` needs one line in its `__init__.py`: `httpp.bridge_logger("libspeech")`. It needs no Python callback, no `set_log_*` and no `_log.py` of its own.
- `logging.Logger.setLevel` is replaced (by a function that calls the original) in every process that makes a bridge. A level set while no bridge exists is picked up when the bridge is made.
- **Not verified:** Windows and macOS; Python 3.13 and free-threaded builds. A library thread that logs in the middle of interpreter finalization can still reach `PyGILState_Ensure` between the `Py_IsInitialized()` check and the call (the window is the same as ADR-0018's); in an experiment a thread that was still logging when the interpreter exited aborted the process, so a library that owns such threads should stop them in its own `atexit` hook.
