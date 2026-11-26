# ADR-0112: A stopped server must be freed by reference counting; the test suite must not need many Python threads

- **Status:** decided and built; verified on Linux with CPython 3.12, free-threaded 3.14.6 and free-threaded 3.15.0b4 (953 Python tests pass on each, plus 27 lifetime tests that fail on the old code); **not yet run on the 32-bit Windows jobs** (needs a CI run)
- **Date:** 2026-11-26
- **Decided by:** the assistant (the owner left the choice to it)

## Context

The `cp314t-win32` wheel job (32-bit free-threaded CPython, Windows) failed in its own test run: `MemoryError` that Python could not print, and a hang inside `threading.Thread.start()`. A first idea, to skip that wheel, was dropped: it would have hidden the problem. The address-space trace in `tests/python/conftest.py` (`MEMTRACE`) showed what happened: at 87 % of the run about 150 MiB of address space were free and about 1.9 GiB in use, falling test by test.

## Finding 1: stopped servers were never freed

On Linux, free-threaded 3.14.6, 100 started and stopped servers left **100 `Server` objects alive** (`weakref`); only `gc.collect()` freed them. RSS grew about 107 KiB per server (about 3 KiB with the GIL). It is a reference cycle that exists on every build (the lifetime tests fail on CPython 3.12 too): the core keeps every handler and hook in `app._handlers`, and

- the closures made by `_adapt` and `_adapt_async` held the `Server` (for its exception handlers, hooks and timeout),
- the `/openapi.json` route held the `Server`,
- `set_before_hook(self._run_before)`, `set_after_hook(self._run_after)` and `set_error_hook(self._run_error)` were bound methods of the `Server`.

Each is `Server -> _handlers -> handler -> Server`. Reference counting never frees it; the cycle collector does, but on free-threaded CPython it runs late, so the stopped servers of a whole test suite piled up with everything they hold.

## Decision 1: handlers hold a shared state object, not the Server

- `_RequestState` holds what serving a request needs: the before / after hooks, the error and exception handlers, the handler timeout, the test-client sink; it also holds the hook runners (`run_before`, `run_after`, `run_error`), `exception_handler_for` and `unhandled`. The `Server` points at it (`self._request_state`), every handler and hook points at it, and it points at neither. `Server._exception_sink` stays, as a property on it (the test client uses it).
- `_OpenAPIDocument` holds what the OpenAPI document is built from and the cache; the `/openapi.json` route holds it, and `Server.openapi()` asks it. The `Server` resets it when a route is added.
- The hooks given to the core are bound methods of the `_RequestState`. `_adapt` and `_adapt_async` take the `_RequestState` instead of the `Server`.
- There is no `weakref` in the fix and no special case: a request in flight keeps what it needs alive through the state object, whatever happens to the `Server`, and the behaviour of every hook and handler is the same as before.

### Rejected on evidence (each was built and measured)

- **`weakref.proxy(app)` in the closures:** `ReferenceError: weakly-referenced object no longer exists` in a task that finished after the app was dropped (seen in a full run on free-threaded 3.14.6).
- **A strong reference taken in each request's frame:** a kept traceback (a log record, a task result) holds the frame and so the app; the lifetime tests failed depending on the order of the earlier tests.
- **`weakref.ref` asked through small helper functions:** it worked (all tests passed) but left a degraded path when the app is dropped while a request runs (no exception handler, no after hook, a hook that answers `None`), which is silent. The state object has no such path.
- Skipping the `cp314t-win32` wheel; `gc.collect()` after each test; a `close()` that clears `_handlers`: they hide the leak or leave it for every user who forgets.

## Finding 2: the second CI log was a different limit, not a leak

After Decision 1 (in its `weakref` form) the next run, `cp315t-win32`, hung at 49 % in `Thread.start()` of `test_async_handlers_do_not_hold_worker_threads`, with **97 Python threads alive**. Before the fix the same kind of hang came at thread 14 (87 % of the run), so the fix gave a large margin. This test opens 100 client threads (`ThreadPoolExecutor(100)`), and a Python thread is expensive where the address space is small. Measured on Linux 64-bit (100 threads that touch the allocator, address space reserved):

| CPython | per thread | 100 threads |
|---------|-----------|-------------|
| 3.12 (GIL) | 13 MiB | 1316 MiB |
| 3.14.6 free-threaded | 44 MiB | 4388 MiB |
| 3.15.0b4 free-threaded | 75 MiB | 7459 MiB |

The allocator of free-threaded builds reserves much more address space per thread, and 3.15 more than 3.14. With the address space capped at 2 GiB (`ulimit -v`) on 3.15.0b4t the old test timed out in `Thread.start()` as in CI, and the new one passes. The CI log of 97 threads in 2 GiB suggests about 21 MiB per thread on 32-bit (an estimate from the log, not measured).

## Decision 2: the 100 concurrent requests come from one thread

`get_all_at_once` in `tests/python/test_httpp_routes.py` opens the connections, sends the requests and reads the answers from the calling thread with `selectors`. What the test checks (100 requests that each await 0.5 s end together in about 0.5 s, so the server holds no thread per request) is unchanged; it does not need 100 client threads. A control test runs 32 requests that sleep 0.25 s in a plain `def` handler against a pool fixed at 4 threads and asserts that it takes more than 1.5 s, so the helper is known to see a server that holds a thread per request. The other thread-heavy tests (16 and 24 threads) are left as they are.

## Tests

- `tests/python/test_server_lifetime.py` (27 tests): the cycle collector is off and the server must be dead right after `del app`, with no waiting. 24 setups cover sync and async routes, a router, before / after hooks (also `async`), an error handler, an exception handler, a hook that answers for the route, a failing route, a websocket route, startup and shutdown hooks, a dependency, background tasks, `serve_directory`, `Swagger`, `FileBrowser` and seven middleware classes; two cases use `TestClient` (one with an opened websocket). Each setup also asserts the status its `/r` must answer: a first version only checked freeing and did not notice an async route that never answered. 26 of the 27 fail on the old code; with the async route made to fail on purpose 2 fail.

## Not verified

- The 32-bit Windows jobs with the final design: `MEMTRACE free=` should stay flat from test to test; the `cp314t-win32` and `cp315t-win32` jobs must pass. The trace stays in `tests/python/conftest.py` until then.
- That no other test runs out of address space on 32-bit. The tests that start 16 and 24 threads should fit (about 97 threads fit at 49 % of the run) but were not run there.
- The per-thread cost on 32-bit (estimated above, not measured) and the cost of the change in speed: the hooks are bound methods of one object as before and the adapters read their state from it instead of from the `Server`, so none is expected; it was not benchmarked.
- A run of the whole suite on Linux under an address-space cap is not usable: free-threaded 3.15 reserves almost the whole cap at start-up, and the run ended in a segmentation fault in the interpreter. It only served to reproduce the hang of the 100-thread test.
