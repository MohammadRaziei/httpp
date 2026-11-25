# ADR-0112: A stopped server must be freed by reference counting, not by the cycle collector

- **Status:** decided; fix verified on Linux (CPython 3.12 and free-threaded 3.14.6); **not yet verified on `cp314t-win32`**, where the problem showed up (needs a CI run)
- **Date:** 2026-11-25
- **Decided by:** the assistant (the owner left the choice to it)

## Context

The `cp314t-win32` wheel job (32-bit free-threaded CPython 3.14.8, Windows) failed in its own test run. Three hook tests in `test_server_features.py` failed with `MemoryError` that Python could not print, and a later run hung in `Thread.start()`. A first idea, to skip that wheel, was dropped: it would have hidden the problem, not found it.

What the CI log of the traced run (`MEMTRACE` lines in `tests/python/conftest.py`) showed:

- At 87 % of the run the process had **152 to 156 MiB of address space free and about 1.9 GiB in use**, falling test by test. The test after that stopped inside `threading.Thread.start()`: no room for another thread.
- Python knew 9 threads, but the stack dump listed about a hundred native threads with `<no Python frame>`: threads the core started (the pinned thread states of `httpp_py_pin_thread`) that were still there.

## Finding

On Linux, free-threaded CPython 3.14.6 (64 bit), the same effect is visible without any Windows:

- After 100 started and stopped servers, **100 `Server` objects were still alive** (`weakref`), and a `gc.collect()` was the only thing that freed them. RSS grew about 107 KiB per server (about 3 KiB per server on CPython 3.12 with the GIL).
- The cause is a reference cycle: the core keeps every handler and hook in `app._handlers`, and
  - the closures returned by `_adapt` and `_adapt_async` hold `app` (for `_exception_handler_for`),
  - the `openapi_json` route closure holds `self`,
  - `set_before_hook(self._run_before)`, `set_after_hook(self._run_after)` and `set_error_hook(self._run_error)` are bound methods of `self`.
  Each one is `app -> _handlers -> handler -> app`. Reference counting never frees it. The cycle collector does, but on free-threaded CPython it runs late, so the servers of a whole test suite pile up, each with its C++ server, its buffers and (on Windows) whatever threads it started.

## Decision

- Everything that is stored in `app._handlers` must not hold the app strongly, and no frame of a request may hold it either.
  - `_adapt` and `_adapt_async` keep a `weakref.ref(app)`. What a request needs from the app (the exception handler, whether after / error hooks exist, the handler timeout) it asks through three small module functions, `_on_exception`, `_wants_after` and `_timeout_of`, so the app is only ever held inside a call that has already returned.
  - The three hook methods are handed to the core through `_weak_hook(...)` (a `WeakMethod`); `_add_docs` keeps a `weakref.proxy(self)` (that route only runs while the server, and so the app, runs).
  - If the app is gone while a request is still running (the app was dropped), the request ends the plain way: no exception handler (the core logs the error and answers a plain 500) and no after / error hooks, and a hook that fires then answers `None`, the neutral answer of every hook kind. The core only starts a request while the app that owns it is alive.
- Two earlier versions were dropped on evidence. A plain `weakref.proxy` for the whole life of the closure gave `ReferenceError: weakly-referenced object no longer exists` in a task that finished after the app was dropped (seen in a full run on free-threaded 3.14.6). A strong reference taken when a request starts fixed that, but any kept traceback (a log record) holds the frame of the request and so held the app: the lifetime tests failed in a pattern that depended on the order of the earlier tests. Resolving the app only inside short helper calls avoids both.
- `tests/python/test_server_lifetime.py` switches the cycle collector off and asserts that a stopped server is dead right after `del app`. It covers every way a server is given something to keep: sync and async routes, a router, before / after hooks (also `async`), an error handler, an exception handler, a hook that answers for the route, a failing route, a websocket route, startup and shutdown hooks, a dependency, background tasks, `serve_directory`, `Swagger`, `FileBrowser`, `TestClient` (also with an opened websocket) and the seven middleware classes (CORS, TrustedHost, GZip, HTTPSRedirect, ProxyHeaders, BasicAuth, RateLimit). The request goes through each, so the request-time paths are covered too. The test asserts the server is dead right after `del app`, with no waiting. All 24 parametrized cases and the two `TestClient` cases failed on the old code (26 of 27 tests; the 27th checks that hooks still work), on CPython 3.12 as well, so the cycle is real on every build and worst on free-threaded ones; all pass now on 3.12 and on free-threaded 3.14.6.
- The address-space trace in `tests/python/conftest.py` stays until a `cp314t-win32` run confirms the fix; then it is taken out.

## Not verified

- That this brings `cp314t-win32` back to green. The mechanism fits the log (servers that are never freed keep memory and threads), but the CI run has to show it: the `MEMTRACE` free-space number should stay flat from test to test.
- Why about a hundred native threads were alive on Windows while Linux shows 3 after the same loop. A server that is alive may keep its pool there; not looked at.
- Cycles in code the test file does not reach: a user's own handlers that hold the app on purpose (a closure over `app` in their code is theirs). The test file is the place to add a case when a built-in is found to hold a server.

## Rejected alternatives

- Skipping the `cp314t-win32` wheel: hides the failure; rejected by the owner and by the evidence (the leak is not specific to that wheel).
- `gc.collect()` after every test or a lower GC threshold in `conftest.py`: makes the suite green and leaves the leak in every program that starts and stops servers.
- A `close()` that clears `_handlers`: needs every user to call it; the cycle would remain for the one who forgets.
