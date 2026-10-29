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
(`set_thread_pool(base, max, grow_after)`, Python `grow_after_ms`); a step adds one thread per 500 us of the wait (20 threads for 10 ms; one for the C++ default, as before), as far as jobs are
queued, so a burst of 64 blocking handlers gets its threads in 3 steps. The C++ default wait stays 500 us. Not yet measured on 16 threads: `make bench` has a `default` row
next to the fixed pools.

## Addendum 3: result of the pool change on 16 threads (2026-10-28, Python 3.14 with a GIL)
CPU-bound `def`: 19.9k req/s, p99 3.8 ms with the new default (it was 10.9k and 26 ms), the same as `async def` (19.3k) and 1.27x uvicorn (15.6k).
It beat a fixed pool of 1 (15.8k). On the free-threaded build the doubling growth step of the first version of this change cost
CPU-bound `def` about a fifth at one I/O thread (46k against 60k), so growth is back to one thread per 500 us of waiting (the step above).

## Addendum 4: the first full `make bench` on 16 threads (2026-10-28, i7-13620H, powersave, Python 3.14.4 GIL and 3.14.7t)
Settings: server CPUs 0-8, `oha` on CPUs 9-11, 64 connections, 3 x 5 s, medians. Only the `hello` and `cpu` groups were in this file: the JSON,
FastAPI, one-connection and fixed-rate groups were not run, so the p99.9 question is still open.

What the data confirms:
1. **GIL, CPU-bound `def`:** 19.9k req/s, p99 3.75 ms with the default pool, the same at 1, 2 and 4 I/O threads (19.9k / 19.7k / 19.8k).
   Fixed pools lose as in addendum 2 (1: 15.8k, 4: 11.5k, 16: 11.8k). uvicorn 15.6k. Nothing to change.
2. **Inline `async def` stays off by default.** 1 I/O thread: 120k against 161k (GIL) and 120k against 164k (no GIL). 2 threads: 262k against 278k
   (GIL), 211k against 304k (no GIL). At 4 threads it ties (284k against 285k, 340k against 344k) and only saves CPU (10.8 against 18.0 us/req on 3.14t).
3. **A GIL build has one loop, and it caps `async def` at about 280k** (278k at 2 I/O threads, 285k at 4), while `def` keeps scaling (315k, 366k):
   the I/O threads answer without the loop. On 3.14t `async def` scales with the lanes: 164k / 304k / 344k, `async` + `await` 140k / 256k / 344k.
4. **C++ stops at about 370k to 390k** with 4 and with 8 I/O threads (364k, 382k at 8), and the server CPU per request grows with the thread count
   (7.4, 8.3, 10.6, 13.0 us for `route`). The load generator and hyper-thread siblings are the limit, not the server design.
5. **p99 at 4+ I/O threads** is 2.9 ms for Python `def` and 3.8 to 6.1 ms for C++ at p50 0.05 ms. The same tail in C++ says it is not Python.
   The cause is still unknown (scheduler, load generator, powersave governor): the fixed-rate group has to answer it.

What it does not confirm:
6. **Free-threaded CPU-bound `def` is 46.5k / 55.1k / 72.5k (1 / 2 / 4 I/O threads), not the 60k to 70k of the earlier run.** These are exactly the numbers
   of the version with the doubling growth step, so this file was probably produced before the revert described in addendum 3 (the header did not name the
   tree, so it cannot be proven). The server spends 104 us per request on `def` against 66 us for the same work in `async def`: about 40 us per
   request go to the hop through the worker pool (lock, wake, hand-off). At 4 I/O threads that is 72.5k x 104 us = 7.5 CPUs on 9 hyper-threads, so
   that row is CPU-bound by the pool overhead, and at 1 I/O thread (4.8 CPUs) it is limited by how many workers the pool has reached.
   Decision: no default change without data. The suite now (a) prints a `tree` hash in the header, (b) runs fixed pools of 2, 4 and 8 for the
   free-threaded build next to the default, so the right base size without a GIL can be read from one run.

Benchmark fix: the automatic CPU split gave the server 0-8 and the load generator 9-11, which cuts the hyper-thread pair 8/9 in half. The split is now made of whole
cores (here server 0-7, load generator 8-15 with the efficiency cores).

## Next measurement
`make bench` again with the governor on `performance`, with the new split and with ONLY=hello,json,cpu,latency (the default). Read: free-threaded `def`
CPU-bound at 1 I/O thread (about 60k expected from the current tree), its fixed-pool rows, the fixed-rate p99.9 table, FastAPI and JSON rows.

## Addendum 5: the second full `make bench` (2026-10-28 evening, same machine, core-aligned split: server CPUs 0-7, `oha` on 8-15)
Tree hash f08eea4db0 (not the hash of the copy this was written from: the owner's working tree has other files; `worker_pool.hpp` matches).
This run replaces addendum 4 where they disagree.

**The CPU split mattered a lot.** The first run shared hyper-thread pair 8/9 between server and load generator and gave `oha` only 3 CPUs. With whole cores
and 8 CPUs for `oha` every row moved: uvicorn hello 65k to 84k, httpp `def` at 1 I/O thread 170k to 217k, C++ at 8 I/O threads 392k to 824k.
Consequences, all corrections of earlier text:
- Addendum 4 point 4 ("C++ stops at about 390k") was the load generator. C++ now scales 213k / 375k / 542k / 701k (824k `route_io`) for 1 / 2 / 4 / 8 I/O threads.
- The p99 of 3 to 6 ms at 4+ I/O threads (addendum 4 point 5) is gone: 0.3 to 0.4 ms at every thread count, p99.9 about 1 ms at half load. It was the benchmark setup, not httpp.
- **The GIL pool effect on throughput does not reproduce.** CPU-bound `def` with a GIL: default 23.9k, pool 1 / 4 / 16 23.0k / 22.6k / 22.4k. Only the tail differs:
  p99 3.1 ms (default), 3.7, 5.5, 11.1 ms. So the 2x throughput loss of addenda 1 to 3 (11k against 20k) came from the old split. The pool default stays, justified by p99 only
  (3.1 against 11.1 ms) and by the 6 percent throughput gap to a pool of 16.

What the run shows:
1. Python `def` peaks at 4 I/O threads: 516k (GIL and no GIL; C++ 542k); at 8 it falls to 416k to 425k while C++ keeps rising (the server has 4 physical cores, and Python adds pool threads).
   So `io_threads = min(4, CPUs // 4)` stays.
2. Python `async def` under a GIL is capped by the one loop: 314k / 310k / 278k for 2 / 4 / 8. On 3.14t it scales: 310k / 481k / 459k.
3. Free-threaded CPU-bound `def` is back at 65k to 72k (65.0k / 67.9k / 68.4k / 71.8k for 1 / 2 / 4 / 8): the revert of the doubling step is confirmed.
   A pool that starts at 2 threads gave 76.4k at 1 I/O thread (against 65.0k for the default, which starts at max(8, CPUs) = 16; pools of 4 and 8 equal the default; sd under 1.3 percent). Not adopted yet: it is one row at one I/O thread.
   The suite now also runs the fixed pools at 4 I/O threads.
4. Inline `async def`: 1 I/O thread loses (159k against 198k GIL, 150k against 198k no GIL); 2 threads loses on no GIL (262k against 310k), ties on GIL (302k against 314k). On 3.14t it is not monotone: 346k at 4 threads against 481k, but
   557k at 8 against 459k. Unexplained; the default (off with more than 2 CPUs) stays. One connection: inline has the best latency (68.8k req/s, p50 0.01 ms against 57.9k for `def`, 48.2k for `async def`; C++ `route_io` 84.9k).
5. JSON on 3.14t: `def JSON` stops at about 300k from 2 I/O threads (318k / 298k / 284k) while `async def JSON` reaches 442k at 4 threads (hello `def` reaches 517k, so it is not the pool hand-off alone).
   Suspects, not tested: refcount contention on shared objects in the free-threaded build, or the pool queue lock. Needs a profile (perf) before any change.
6. FastAPI + uvicorn: 22k hello, 17k JSON. httpp `def` is 9.7x (hello) and 11.8x (JSON) at 1 I/O thread, 23x at 4. Bare ASGI uvicorn 84k.
7. Fixed-rate (half of each maximum): p99.9 0.8 to 1.2 ms for Python at 1 and 8 I/O threads, 0.8 ms for C++ at 412k req/s.

Build hygiene found while reading the log: `-Wreorder` in `worker_pool` (member order against the constructor list; harmless, fixed), uv hard-link warnings (the project is on another filesystem than the uv cache; the
Makefile now sets `UV_LINK_MODE=copy`), CMake finding the conda Python (only used by the `liburlparser` configure, never for the measurement; the Makefile now passes the venv Python, which then needs `jsonschema` and `jinja2` for the mbedtls code generator: they are installed next to cmake and ninja, so the configure no longer depends on whatever Python the machine has), mbedtls deprecation notes (third party, silenced for the bench build).
Not touched: the configure step of `liburlparser` downloads the public suffix list and rewrites the tracked `src/third_party/liburlparser/src/public_suffix_list.dat`.

## Addendum 6: a crash found by the second bench (2026-10-29)
The full run (tree 9c83b41592) had two rounds that lost the server: Python `def` with a GIL at 8 I/O threads (131k req/s and 606,771 errors in the round: the server
died about 1.5 s into it) and C++ `route` (worker pool) at 4 I/O threads (a round with 0 answers and 2,528,812 errors). About 150 server starts, so rare, and only with several I/O threads.
The suite did not keep the exit code or the server's stderr, so the first look was blind. It now prints the exit code, the error kinds and the stderr tail when a run has errors.

Cause, found with a ThreadSanitizer build of the core under `oha` load (4 and 8 I/O threads, `route` and `route_io`): `http_date()` in `async_server.cpp` kept its per-second cache
(`cached`, `text`) in function-level statics, documented as "loop thread only". Since ADR-0024 there is one loop thread per I/O thread, so they all wrote and read the same
`std::string`. Torn reads were harmless in practice, but the first answers of a fresh server can run the fill on two threads at once (`text.empty()` is true for both): two
assignments to one heap string are a double free. TSan: 5 warnings per run before, 0 after, in three runs at 4 threads and one each (pool and inline route) at 8.
Fix: the cache is `thread_local`. Regression test: `the_date_header_is_well_formed_when_every_io_thread_answers_at_once` (20 fresh servers, 16 clients each). It can only fail
by luck in a normal build; the race itself is visible with `-fsanitize=thread`. ASan found nothing in six 5 s runs.

Not shown by the evidence: that this race is the cause of both lost servers. It is the only race TSan reported, and it fits (rare, at startup or under several loops, pool and inline paths),
but no core file was kept. The next `make bench` decides: the rows with errors must be gone, and if one appears again the new report says how the server died.

Other reading of the run, same as addendum 5 unless stated: GIL pool rows are flat (22.4k to 23.2k) at 1 and 4 I/O threads, only p99 differs. On 3.14t a pool that starts at 2 threads is +17 percent at 1 I/O threads (76.7k against
65.3k; the default for a free-threaded Python is 1 I/O thread, so this is the default configuration) and equal at 4 (69.8k against 68.0k). Half-load p99.9 is 0.8 to 1.3 ms for Python and 0.8 ms for C++ at 396k req/s.
