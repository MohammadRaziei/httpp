# ADR-0021: Pool threads keep their Python thread state; first two-core numbers

- **Status:** accepted
- **Source:** the two-core measurement session, `src/bindings/python/httpp_cy.pyx` (`httpp_py_pin_thread`), `benchmarks/`

## Context

`def` handlers run on C++ pool threads that Python does not know. Every handler call used `with gil`, which is
`PyGILState_Ensure` / `PyGILState_Release`. For a thread without a Python thread state that creates a state on every request and
destroys it again. With a GIL that costs about half of the request rate; on free-threaded CPython (3.14t) it made `def` routes
about 5 times slower (1.9k req/s on `hello`, against 9.4k on 3.13).

## Decision

- Each pool thread calls `PyGILState_Ensure` once and never releases it (`httpp_py_pin_thread`), then detaches. Later `with gil`
  blocks only attach and detach the same state. It is called at the start of the five dispatchers (`def`, raw, before, after, error hooks).
- A sync streaming generator is closed when the response ends or the client hangs up (`provider.close = gen.close`, called by
  `_stream_release`). On free-threaded CPython an object released by another thread is freed only when the thread that created
  it next runs Python, so without this its `finally` (an open file, a connection) could wait for the next request on that thread.
- The worker pool wakes sleeping threads last-in first-out, wakes at most as many threads as there are queued jobs, and
  grows only when no thread is asleep (`worker_pool.hpp`). Before, one condition variable woke threads in rotation, so a light load
  spread over all 8 pool threads and they fought for the Python lock. An experiment with a fixed pool showed it: `hello` on 3.13 with
  1, 2, 4, 8 pool threads gave 34.9k, 29.8k, 22.5k, 19.9k req/s. A blocked thread still never leaves queued work waiting
  (a chain wake on every dequeue); `tests/cpp/test_worker_pool.cpp` covers it and runs clean under ThreadSanitizer.
- ponytail: the state of a pool thread that exits is freed at interpreter shutdown, not earlier. The pool lives as long as the server.
  Upgrade path: release the pinned state from a thread-exit hook if pools ever shrink.

## Rejected alternatives

- Keep per-request states on GIL builds and pin only on free-threaded builds: the GIL build also doubled (9.4k to 17.9k), so no reason.
- Call `gc.collect()` on release to flush the owning thread's queue: a stop-the-world pass per streamed response.

## Consequences

Numbers (2 cores, `oha` in a separate process on the same machine, 32 connections, median of 3 rounds of 4 s, one worker,
server and load generator share the cores, so only ratios inside one run mean anything; run to run it moves about 10 percent):

| `hello` req/s | FastAPI + uvicorn | uvicorn (bare ASGI) | httpp `def` | httpp `async def` |
|---|---|---|---|---|
| CPython 3.13 (GIL) | 4.6k | 19.3k | 16.2k | 21.0k |
| CPython 3.14.6t | 4.4k | 17.3k | 19.5k | 24.4k |

| `item` (`item_id: int`, JSON body) | FastAPI | uvicorn | httpp `def` | httpp `async def` |
|---|---|---|---|---|
| 3.13 | 3.6k | 12.4k | 12.1k | 17.7k |
| 3.14.6t | 3.2k | 14.8k | 17.1k | 19.7k |

The last of 300 routes: FastAPI 1.2k (3.13) and 1.5k (3.14t); uvicorn's bare app has no router. httpp `def` 13.1k and 16.9k, `async def` 21.0k and 24.1k.

CPU-bound handler (about 3 ms of pure Python, `benchmarks/cpu_bench.py`), 1 and 8 connections:

| req/s | uvicorn | httpp `def` | httpp `async def` |
|---|---|---|---|
| 3.13 | 284 / 337 | 307 / 308 | 309 / 389 |
| 3.14.6t | 327 / 387 | 350 / **730** | 349 / 398 |

- Only `def` handlers on free-threaded CPython use both cores (about 2.1 times from 1 to 8 connections). `async def` handlers share one loop
  thread, so they cannot gain from free-threading (a small gain on 3.14t is probably I/O on the Asio thread; not investigated).
- After the pool change (same session, noise about 10 percent): `def` `hello` 21.8k to 22.5k on 3.13 and 24.6k to 26.0k on 3.14t, against
  uvicorn 15.0k to 18.7k and 18.0k. `def` `item` is 12.5k on 3.13 (uvicorn 14.4k) and 17.7k to 18.6k on 3.14t. The CPU-bound numbers did not change
  (3.14t, 8 connections: 640 to 740 req/s, three runs). Open: `def` `item` costs about 10k req/s more than `def` `hello` on 3.13, and
  `async def` pays much less for the same route (17.3k): the difference is in the Python per-request work of the `def` path, not yet profiled.
- Not verified: Windows, macOS, more than two cores, a real network (client and server share the machine), FastAPI with `response_model`
  or dependencies, and a longer thread-safety review of module-level Python state under free-threading.
- `test_pool_thread_state.py` guards the reuse. On 3.14t `test_a_streamed_generator_is_released...` no longer waits for the weakref
  to fire (the object may live until the creating thread runs Python again); it checks the generator's `finally` instead.

## Related

ADR-0009 (hot path), ADR-0013 (free-threaded wheels), `Architecture.md` risk 1 and risk 8.
