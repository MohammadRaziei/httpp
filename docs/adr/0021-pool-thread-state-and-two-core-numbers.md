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
- **A job does not wake a second thread while another thread is running or about to run.** That thread usually finishes in
  microseconds and takes the next job itself; two threads handing the Python lock to each other cost more than they gain
  (the profile showed about 10 us of Python per `item` request, `json.dumps` a quarter of it, yet 80 us of wall time per request:
  the rest was the Python lock changing hands). A scout thread, asleep unless a job is waiting behind a busy thread, wakes or adds a
  thread when a job has waited 500 us (`help_after`), so a blocked handler delays queued work by about that long and not more
  (`a_long_job_gets_help_after_a_short_delay`). Under a saturating CPU-bound load the scout adds one thread per 500 us until
  every core is busy, so 3.14t still scales.
- ponytail: the state of a pool thread that exits is freed at interpreter shutdown, not earlier. The pool lives as long as the server.
  Upgrade path: release the pinned state from a thread-exit hook if pools ever shrink.

- **`async def`: one shared deadline timer and a prebuilt JSON encoder** (`_Deadlines`, `_dumps` in `server.py`). A profile of the loop
  thread (py-spy, GIL samples) put `call_later` at 9 percent, `create_task` at 9 percent and `json.dumps` at 14 percent. Switching
  `handler_timeout` off alone gave +18 percent (`hello`, 23.5k to 27.9k) and +25 percent (`item`, 16.6k to 20.6k). Now one timer watches all
  in-flight requests and a finished request is forgotten at once (`test_finished_async_requests_leave_no_deadline_behind`).
  Measured in a later run on a faster machine state (uvicorn itself doubled, so compare ratios only; 2 cores, 32 connections):
  `async def` `hello` / `item` on 3.13 47.5k / 43.7k against uvicorn 34.0k / 22.8k (1.4x / 1.9x, before 1.1x / 1.4x), on 3.14t
  48.7k / 44.8k against 31.9k / 26.1k; `def` 61.9k / 55.3k and 62.1k / 57.2k.

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
- After the delayed-help change (2 cores, 32 connections, median of 3 rounds, noise about 10 percent), `def` `hello` / `item`:
  3.13 35.9k / 24.9k (uvicorn 17.2k / 11.9k, `async def` 25.2k / 19.8k), 3.14t 33.2k / 29.8k (uvicorn 18.2k / 14.0k, `async def`
  22.8k / 21.8k). CPU-bound on 3.14t, 8 connections: 751 req/s. `def` is now faster than `async def` here: the `async def` path
  had a different cost: per-request timer and task creation, see the `async def` item below.
  Eight concurrent 200 ms blocking handlers finish in 0.21 s (the very first batch after start-up takes 0.49 s, cause not looked into).
- Before the delayed-help change (LIFO wake only): `def` `hello` 21.8k to 22.5k on 3.13 and 24.6k to 26.0k on 3.14t, against
  uvicorn 15.0k to 18.7k and 18.0k. `def` `item` is 12.5k on 3.13 (uvicorn 14.4k) and 17.7k to 18.6k on 3.14t. The CPU-bound numbers did not change
  (3.14t, 8 connections: 640 to 740 req/s, three runs). That `item` gap was the Python lock changing hands, not the Python work.
- Not verified: Windows, macOS, more than two cores, a real network (client and server share the machine), FastAPI with `response_model`
  or dependencies, and a longer thread-safety review of module-level Python state under free-threading.
- `test_pool_thread_state.py` guards the reuse. On 3.14t `test_a_streamed_generator_is_released...` no longer waits for the weakref
  to fire (the object may live until the creating thread runs Python again); it checks the generator's `finally` instead.

## I/O-bound handlers (10 ms wait), `benchmarks/io_bench.py`

Why `async def` is slower than `def` on trivial handlers but not in general. Syscalls per request (`strace -c -f`, `item`, 32 connections):
uvicorn 3.1, httpp `async def` 3.2, so the extra wake-ups are not the cost (hypothesis H1 rejected; the time cost of a thread
hand-off was not measured). A `def` handler runs on a pool thread with no event loop, Task or callback, which is the likely
reason it is faster on a handler that does nothing (a hypothesis, see the CPU split below). With a 10 ms wait the picture flips (3.13, 2 cores, one run, req/s with p99 in ms):

| connections | 32 | 128 | 512 |
|---|---|---|---|
| uvicorn (asyncio loop) | 2.9k, 13 | 10.5k, 15 | 25.9k, 41 |
| httpp `def` (`time.sleep`) | 3.0k, 12 | 3.1k, 65 | 3.2k, 167 |
| httpp `async def` (`asyncio.sleep`) | 2.8k, 13 | 10.2k, 16 | 32.5k, 25 |

A blocking `def` handler is capped by the pool (about 32 threads over 10 ms, 3.1k req/s) and its p99 grows with the connections;
`async def` keeps scaling because a waiting coroutine holds no thread. On 3.14t httpp gives the same numbers (2.8k, 10.5k, 31.6k);
the uvicorn row there (1.9k, 5.1k, 5.0k) is not comparable, because uvloop was not installed in that venv.
Rule of thumb for the README: `def` for CPU work or blocking libraries, `async def` for waiting on the network.

### Where the `async def` gap comes from (CPU per request, `item`, 32 connections, 2 cores, 3.13)

Per-thread CPU from `/proc/<pid>/task/*/stat` over 6 s: `async def` costs about 29.5 us of server CPU per request, `def` about 20.1 us.
Loop thread (Python) 15.9 us against 8.8 us of Python work on the pool thread for `def`, so about 7 us of the 9.4 us gap is on the Python
side (asyncio machinery); the Asio thread costs 13.6 us against 11.3 us, about 2.3 us (one extra `post` and wake-up per answer).
Neither thread is at 100 percent because `oha` shares the two cores. With `py-spy --gil --native`, about 31 percent of the loop thread's
GIL time is inside the compiled `_drain_inbox` (inbox pop, `_Done` and responder copy, `_new_raw`), and about 12 percent is a libc private
lock wait (not identified; glibc `arena_max=1` and a larger tcache changed nothing within noise). Starting a task eagerly
(`asyncio.Task(..., eager_start=True)` on 3.12+, `dispatch` in `server.py`) gained 2 to 8 percent on `hello` / `item` (3 alternating
rounds, inside the noise).

Nanosecond timers inside the compiled module (temporary build, `async def` `item`, loop thread, per request): `inbox_pop` 2.5 us,
making `_Done` 0.24 us, making the raw request 0.13 us, the Python dispatch call 15.3 us (of which `_fill_response` 0.95 us and the
`send` into C++, which posts to the Asio thread, 2.9 us). So about 5.7 us of about 18 us is the Python/C++ boundary. The inbox popped one
item per lock; `_drain_inbox` now takes a whole batch under one lock (`httpp_py_inbox_pop_many`, same arming rules). A/B with the old and
new compiled module, 3 alternating rounds, median: `hello` 46.1k to 51.5k, `item` 40.5k to 47.3k req/s (about 12 and 17 percent). The
remaining boundary cost is the `send` (one `asio::post` and wake-up per answer): batching the answers is option C of risk 1, an internal
protocol change that still needs the owner's decision.

Python side after that (all in `server.py`): `_to_response` answers an exact `str`, `dict` or `list` without its general checks (a test pins
that the answers are the same), and the `handler_timeout` deadline is registered only for a task that did not finish at once. A/B with the
`server.py` from before this session and the new compiled module, 3 alternating rounds, median (the box was slower than in the earlier
runs, compare only inside this pair): `hello` 39.6k to 47.4k, `item` 35.6k to 43.1k req/s (about 20 percent), better in every round.

### Stress run and the decision on option C

`benchmarks/stress.py 20 16` (16 client threads, sync, async, streaming, failing and slow routes, clients hanging up early) with the code
above: 3.13 two runs of about 51k answers, 3.14t three runs of 74k to 77k answers, no wrong answer, no unexpected line on stderr, server
alive. The first runs of the day failed, but the harness was at fault: the server's stderr went to a pipe nobody read, so after about
1100 requests (64 KB of log lines) the server blocked on its own log; stderr now goes to a file. A full log pipe stalls any server that
logs to it; httpp does not guard against that.

Option C (answers of one loop turn handed to C++ in one call) is not taken for now: the measured cost it could save is the `send` (about
2 us of about 18 us on the loop thread, one extra wake-up per answer), it changes the internal `done` protocol, and it adds latency for the
first answer of a burst. The owner delegated the choice ("decide the right thing"); reopen it if `async def` throughput becomes the limit.

## Related

ADR-0009 (hot path), ADR-0013 (free-threaded wheels), `Architecture.md` risk 1 and risk 8.
