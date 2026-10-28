# ADR-0025: defaults after the first many-core measurement (i7-13620H, 16 threads)

## Status
Accepted (2026-10-28). Amends ADR-0023 (inline `async def` is no longer the default on machines with more than 2 CPUs) and
ADR-0024 (the Python default for `io_threads` is no longer always 1).

## Measurements
`benchmarks/suite.py`, Ubuntu, kernel 7.0, powersave governor, server on CPUs 0-8, `oha` on 9-11, 64 connections, 3 x 5 s, medians.

| hello, req/s | 3.13 (GIL) | 3.14t (no GIL) | C++ |
|---|---|---|---|
| uvicorn bare ASGI | 63k | 64k | |
| httpp `def`, 1 / 2 / 4 I/O threads | 170k / 309k / 344k | 169k / 316k / 383k | `route` 174k / 308k / 376k |
| httpp `async def` (inline), 1 / 2 / 4 | 126k / 265k / 288k | 120k / 213k / 329k | `route_io` 177k / 306k / 397k |
| httpp `async` + `await`, 1 / 2 / 4 | 158k / 209k / 199k | 160k / 283k / 347k | |

CPU-bound handler (about 60 us of Python), req/s:

| | 3.13 (GIL) | 3.14t (no GIL) |
|---|---|---|
| uvicorn | 13k | 15k |
| httpp `def`, 1 / 4 I/O threads | 7.5k / 7.6k | 63k / 70k |
| httpp `async def`, 1 / 2 / 4 | 15k / 16k / 16k | 17k / 33k / 63k |

## What this says
1. **I/O threads scale.** Two I/O threads give 1.8x, four 2.0x; beyond that the load generator (3 CPUs) is the limit: C++ stops at about 395k
   with 4 or 8 threads, and Python `def` reaches 87 to 97 percent of C++. Python is not what limits the server here.
2. **Inline `async def` loses at saturation when cores are free.** It costs less CPU (7.9 us) but does parse, handler and answer on one
   thread: 1 / 7.9 us is exactly its 126k. The hop to the loop thread makes a two-thread pipeline (158k). In the sandbox, with 2 cores shared
   with the load generator, the saved CPU won (+30 percent), and with a single connection inline halves latency (0.04 against 0.08 ms there).
   So: inline is the default only with 2 CPUs or fewer; `inline_async=True` forces it, and the suite measures both.
3. **GIL Pythons and CPU-bound `def`: worker threads fight for the GIL.** 179 us of CPU per request against 67 for the same work in
   `async def`, p99 34 ms, slower than uvicorn. Not fixed here; see "Next".
4. **Free-threaded Python pays where it can:** CPU-bound `def` 63k to 70k (7x the GIL build), CPU-bound `async def` scales with the lanes
   (17k, 33k, 63k for 1, 2, 4).

## Decision
- Python `io_threads` default: `min(4, CPUs // 4)` with a GIL (the extra threads parse and answer without the GIL, and there is still one
  loop, so asyncio's atomicity rule is unchanged); 1 on a free-threaded Python (a loop per I/O thread changes what shared state in async handlers must
  withstand, so it is asked for) and on Windows.
- C++ `io_threads` default stays 1: `route_io` handlers would otherwise have to be thread-safe by surprise. The documentation recommends 2 to 4.
- Python `inline_async` default: on with 2 CPUs or fewer, off otherwise.

## Not verified
Whether the load generator hides gains above 4 threads (it had 3 CPUs). Behaviour on battery or with the performance governor.

## Addendum: the worker pool under a GIL (2026-10-28, 2-core sandbox, Python 3.13)
Fixed worker pool of 1, 2, 4 and 16 on the CPU-bound `def` route: the same req/s (10.0k to 11.6k), p99 9, 11, 15 and 33 ms.
So many workers cost tail latency here; the throughput loss seen on 16 threads (7.5k against 15k) could not be reproduced on 2 cores.
Blocking handlers lose almost nothing with a small starting pool (24 handlers of 0.2 s: 0.22 to 0.24 s with a base of 2, 4 or 16), because the
scout adds threads after 0.5 ms of waiting. Decision: with a GIL the default pool starts at 4 threads (maximum unchanged). The pool also
grows under CPU load (queued jobs wait more than 0.5 ms), so this is a small help only: with the default, p99 was still 25 ms in the
sandbox. A real fix needs the pool to know whether a handler computes or blocks, which it cannot. Free-threaded Python does not have the problem
(63k to 70k req/s). Rerun `make bench ONLY=cpu` on the 16-thread machine to see what the change did there.

## Addendum 2: the pool size on 16 threads (2026-10-28, Python 3.14 with a GIL, CPU-bound `def`, 64 connections)
| pool (max 4 x base) | req/s | us/req | p99 ms |
|---|---|---|---|
| 1 | 16.7k | 81 | 8 |
| 2 | 12.2k | 121 | 11 |
| 4 | 12.2k | 123 | 15 |
| 16 | 10.6k | 140 | 35 |

The same work in `async def`: 19.3k, uvicorn 15.7k. So with a GIL the best pool for CPU-bound work is the smallest, and the "4 threads" of
the first addendum was too many. A blocked handler needs the pool to grow, which a fixed small pool cannot do.
Decision: with a GIL the pool starts with one thread and grows only when a job has waited 10 ms for a free thread
(`set_thread_pool(base, max, grow_after)`, Python `grow_after_ms`); at every growth step the pool doubles, as far as jobs are queued, so
a burst of 64 blocking handlers gets its threads in 6 steps (measured 0.42 s for 64 handlers of 0.2 s on a 32-thread maximum; one
thread per step would have been 0.66 s). The C++ default wait stays 500 us. Not yet measured on 16 threads: `make bench` has a `default` row
next to the fixed pools.
