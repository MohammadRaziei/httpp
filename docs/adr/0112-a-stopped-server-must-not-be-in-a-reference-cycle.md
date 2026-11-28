# ADR-0112: A stopped server must be freed by reference counting; a thread that ends gives its Python thread state back; the test suite must not need many Python threads

- **Status:** decided and built; verified on Linux with free-threaded CPython 3.14.6 and with CPython 3.13 (see Tests); **not yet run on the 32-bit Windows jobs** (needs a CI run). Decisions 1 and 2 were verified earlier on 3.12, 3.14.6t and 3.15.0b4t.
- **Date:** 2026-11-26 (Decisions 3 and 4: 2026-11-28)
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

## Finding 3: native threads kept a Python thread state for ever

After Decisions 1 and 2 the `cp314t-win32` job still hung in `Thread.start()`, now at 93 % of the run (`test_two_clients_for_two_apps_do_not_mix`), with `MEMTRACE free=90MiB used=1957MiB threads=10`. The thread dump listed more than 100 threads, and almost all of them had no Python frame. `threading.active_count()` counts only the threads that `threading` made; a native thread that ran Python code (a worker of the server pool) holds a Python thread state, and `httpp_py_pin_thread` took one with `PyGILState_Ensure` and **never released it** ("freed at interpreter shutdown"). The pool threads that leave after 5 idle seconds, and every pool that is shut down, left their states behind. On free-threaded CPython each state keeps its own allocator pages, which is address space that 32-bit does not have.

Measured on Linux, free-threaded 3.14.6, the whole Python suite (973 tests) with `HTTPP_MEM_TRACE=1`, the number of Python thread states counted through `PyInterpreterState_ThreadHead`:

| code | thread states at the end of the run (maximum) | `threading.active_count()` (maximum) |
|------|-----------------------------------------------|---------------------------------------|
| before this change | **440** | 12 |
| after this change | **17** | 11 |

A loop of 10 servers started and stopped, 8 requests each, leaves +10 states before the change (one per server) and +0 after.

The growth came from these files (states added by their tests): `test_cli.py` 86, `test_async_engine.py` 42, `test_io_threads.py` 30, `test_server_features.py` 28, `test_testclient.py` 26, `test_server_lifetime.py` 23, `test_request_streaming.py` 18, `test_client.py` 17. The earlier idea that `blocking_pool` was a source was wrong: `src/core/internal/blocking_pool.hpp` was not used by anything, so it was deleted.

## Decision 3: a thread httpp started frees its state as the last thing it does (C++ layer)

- `httpp::set_thread_exit_hook(fn)` and `httpp::run_thread_exit_hook()` (`include/httpp/utils.hpp`): one process-wide function, run on every thread httpp starts, on that thread, at the very end of its function. The core knows nothing about Python; the binding installs `httpp_py_unpin_thread` when the module is imported.
- It is run by a local object of the thread function (`detail::thread_exit_scope`, `src/core/internal/thread_exit.hpp`), **not** by a `thread_local` destructor: on Windows a `thread_local` destructor runs inside `DllMain` with the loader lock held, and freeing a state waits for the interpreter, which a thread that is importing a module can hold. Threads covered: the workers of the server pool, the extra I/O threads, the listener thread of `listen`, the network thread of an async client, and the thread of `download::run_async`.
- `worker_pool` takes the function as a constructor argument and runs it **outside its lock**: an idle thread that leaves marks itself as `leaving_` (so two of them cannot both leave and take the pool below its base size), unlocks, runs the hook, locks again and registers as exited. A hook that waits for the interpreter while a running job waits for the pool lock (an `enqueue`) would deadlock otherwise. The idle time became a constructor argument (default unchanged, 5 s) so the tests need not wait 5 s.
- In the binding, a thread that Python already knows (made by `threading`) is not pinned at all, and the state of one that was pinned is released with `PyEval_RestoreThread` and `PyGILState_Release`, whose counter drops to 0, so the state is cleared and deleted. Only functions of the limited API are used (the abi3 wheel targets 3.9).

### Rejected

- **A `thread_local` destructor that releases the state:** the `DllMain` problem above, and the destructor runs after the thread function is over, where the order against other thread-local objects is not ours to decide.
- **Fewer threads (smaller pool bases on 32-bit):** it lowers the number and keeps the leak; it would be a limit that every long-running user still reaches.
- **`PyThreadState` created and deleted for each request:** the reason for pinning (about 5 times fewer requests per second on free-threaded CPython, see `test_pool_thread_state.py`).

## Finding 4: `TestClient` returned before the startup hooks had run

Found by the full run above: `test_with_runs_startup_and_shutdown_once` failed once, on a loaded 2-core machine, with `{'startup': 1, 'shutdown': 0} == {'startup': 1, 'shutdown': 0}` (the value changed between the comparison and the report). `TestClient._start` waited with `socket.create_connection`, but the socket of `bind_to_any_port` already accepts connections (the kernel backlog) while the startup hooks still run in `listen_after_bind`. Reproduced every time with a startup hook that sleeps 0.3 s. The race was there before this change.

## Decision 4: the server says when it is ready

`Server` calls `self._on_ready` (default `None`) on the serving thread after the startup hooks and before it serves; `TestClient._start` waits for that instead of connecting. A request that arrives right after waits in the backlog (`_halt` already repeats `stop()` for a stop that races the start of the loop). `_on_ready` is cleared when the client stops.

## Tests

- `tests/python/test_server_lifetime.py` (27 tests): the cycle collector is off and the server must be dead right after `del app`, with no waiting. 24 setups cover sync and async routes, a router, before / after hooks (also `async`), an error handler, an exception handler, a hook that answers for the route, a failing route, a websocket route, startup and shutdown hooks, a dependency, background tasks, `serve_directory`, `Swagger`, `FileBrowser` and seven middleware classes; two cases use `TestClient` (one with an opened websocket). Each setup also asserts the status its `/r` must answer: a first version only checked freeing and did not notice an async route that never answered. 26 of the 27 fail on the old code; with the async route made to fail on purpose 2 fail.
- `tests/cpp/test_worker_pool.cpp` (3 new): every worker runs the hook once at shutdown (4 for a pool of 4, the scout runs none); a thread that leaves after idling runs it while the base thread stays, and the pool still works; the hook runs without the pool lock (a hook that calls `enqueue` does not deadlock). `tests/cpp/test_thread_exit_hook.cpp` (2): `run_thread_exit_hook` calls the installed function and does nothing without one; a stopped server has run it on each of its workers. The whole C++ suite passes (834 tests).
- `tests/python/test_pool_thread_state.py::test_stopped_servers_give_their_thread_states_back`: 10 servers started and stopped must not add more than 3 thread states. It fails without the hook (a build with the install line removed: "grew from 3 to 13") and passes with it.
- `tests/python/test_testclient.py`: a startup hook that sleeps 0.3 s is done when the client is entered; a failing startup hook is raised by the client.
- `tests/python/conftest.py`: `MEMTRACE` now also prints `states=` (the number of Python thread states) and there is a `python_thread_states` fixture.

## Not verified

- The 32-bit Windows jobs with the final design: `MEMTRACE states=` and `free=` should stay flat from test to test (on Linux the maximum fell from 440 to 17); the `cp314t-win32` and `cp315t-win32` jobs must pass. The trace stays in `tests/python/conftest.py` until then.
- A worker that ends while the interpreter is being finalized: `PyEval_RestoreThread` in the hook can end the thread, so the hook returns early once the interpreter is no longer initialized. Tried on Linux with 3.14.6t and 3.13 (abi3), 3 to 5 runs each, all exit code 0: the main thread exits while a server serves on a daemon thread; the server is stopped from an `atexit` handler; a non-daemon thread stops the server after the main thread is done. Not tried on Windows or macOS.
- That no other test runs out of address space on 32-bit. The tests that start 16 and 24 threads should fit (about 97 threads fit at 49 % of the run) but were not run there.
- The per-thread cost on 32-bit (estimated above, not measured) and the cost of the change in speed: the hooks are bound methods of one object as before and the adapters read their state from it instead of from the `Server`, so none is expected; it was not benchmarked.
- A run of the whole suite on Linux under an address-space cap is not usable: free-threaded 3.15 reserves almost the whole cap at start-up, and the run ended in a segmentation fault in the interpreter. It only served to reproduce the hang of the 100-thread test.
