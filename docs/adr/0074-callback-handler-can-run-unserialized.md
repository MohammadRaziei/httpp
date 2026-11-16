# ADR-0074: `callback_handler` can call its function without the handler's lock

- **Status:** accepted; amends ADR-0057 (the handler's locking rule has one exception)
- **Decided by:** the project owner (asked for the fix after the experiment below)
- **Source:** `include/httpp/logging.hpp` (`callback_handler`, `handler::set_serialized`), `src/core/logging.cpp` (`handler::handle`), `tests/cpp/test_logging_module.cpp`

## Context

`handler::handle` takes the handler's mutex and calls `emit()` with it held (ADR-0050, ADR-0057), so a handler never needs locking of its own. That is wrong for a callback that has to wait for something another logging thread is holding. The case that matters is the Python GIL: the callback takes the GIL (`with gil` in Cython, `PyGILState_Ensure`, ctypes), and a library thread that logs holds the handler's lock while it waits for the GIL. A Python thread that holds the GIL and calls C++ **without releasing it** (a binding that does not use `nogil`) logs through the same handler and waits for the lock. Neither can go on.

An experiment (a small shared library over `httpp::logging`, Python threads through ctypes, C++ threads logging at the same time) showed it on Linux:

- Python threads calling C++ with the GIL released (`CDLL`): no problem, 3600 of 3600 lines, never two callbacks at once.
- Python threads calling C++ with the GIL held (`PyDLL`): deadlock, every time. It does **not** need the callback to block or sleep: a library thread waiting for the GIL is enough.
- A callback that logs again through the same handler: deadlock (a non-recursive mutex locked twice by one thread).

httpp's own Python bridge is safe only because of a rule: every call from Cython into C++ is `nogil`, and the docstring of `set_log_callback` says the callback must not call into httpp. A program that uses `httpp::logging` directly (libspeech is the first) cannot be expected to know that rule.

## Decision

- `callback_handler(fn, bool serialized = true)`. With `serialized = false`, `handler::handle` releases the handler's lock **after the filters ran** and before `emit()`, so `fn` is called while the thread holds no httpp lock. The default is the old behaviour; nothing changes for existing code, including `httpp::set_log_callback` and the Python bridge.
- The flag lives in the handler's private state, set through the protected, non-virtual `handler::set_serialized(bool)`: no new virtual function, no change of the vtable. It is public only through the `callback_handler` constructor, which is race free (the handler is never seen by a logger before the flag is set).
- Consequences for `fn` when it is unserialized: it can run in several threads at once and must serialize itself (taking the GIL does), and it may log again (it must not do that without a limit). A handler that sets the flag must not call `format()` in `emit()`: `set_formatter` replaces the formatter under the lock. `callback_handler` gets the raw record and never formats, so it is safe.
- The filters still run under the lock, as before.

## Rejected alternatives

- **Make every handler unserialized.** `stream_handler` and `file_handler` rely on the lock to write one record at a time.
- **A recursive mutex.** It would fix the second case only; the first is two different threads and two different locks.
- **A queue in httpp (the callback only queues, another thread delivers).** It works (tested in the experiment) but costs a delivery thread or a drain call in every consumer, and lines arrive late. The caller can still build it on top of `callback_handler`.
- **Fix it only in documentation (the `nogil` rule).** That is what httpp does for itself; the experiment shows how little it takes to break it.

## Consequences

- A consumer whose callback takes the GIL should pass `serialized = false`.
- Not verified: Windows and macOS (the experiment and the tests ran on Linux only), and a real Python binding of a consumer; only the shape of the deadlock was reproduced with ctypes.

## Related

ADR-0050, ADR-0057
