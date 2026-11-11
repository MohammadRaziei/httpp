# ADR-0059: The worker pool of a free-threaded Python starts at two threads

- **Status:** proposed until the owner's re-run of the default row (see Verification); amends ADR-0025
- **Decided by:** Claude, on the owner's "continue" (performance and architecture are delegated)
- **Source:** `src/bindings/python/httpp/server.py` (`Server.__init__`), `tests/python/test_io_threads.py`, `benchmarks/suite.py`

## Context

Under a GIL the default pool starts at one thread and grows (ADR-0025). Without a GIL it kept the C++ default: `max(8, CPUs)` threads from the start (16 on the owner's machine), ceiling four times that.
ADR-0025 addendum 4 saw a pool starting at 2 beat it at one I/O thread (76.4k against 65.0k req/s) but called it one row and did not adopt it.

## Measurement (owner's machine, i7-13620H, `performance` governor, Release, Python 3.14.7t, CPU-bound `def` route, 64 connections, 3 rounds, median and range)

| I/O threads | default (16 threads) | pool 2 (ceiling 8) | pool 4 | pool 8 |
|---|---|---|---|---|
| 1 | 58.2k (55.4 to 58.6) | 68.2k (68.0 to 68.3) | 57.5k | 57.6k |
| 4 | 64.0k (61.7 to 65.4) | 66.1k (65.8 to 67.0) | 61.7k | 64.1k |

Pools of 4 and 8 equal the default; only the small start wins. Two I/O threads was not measured for pool 2 (the suite runs the fixed pools at the first and at four I/O threads only).

## Decision

On a free-threaded Python the default pool is `set_thread_pool(2, max(8, CPUs) * 4)` with the usual 0.5 ms growth wait. The ceiling is the old default's, not the 4 x 2 of the measured rows, so blocked handlers still get their threads. `threads=` given by the caller is unchanged. Builds with a GIL are unchanged.

## Consequences

- A handler that blocks (sleep, I/O) on a free-threaded build now waits up to 0.5 ms per extra thread to get one instead of finding it idle. Test `test_blocking_handlers_still_run_together_with_the_small_default_pool` (24 handlers of 0.2 s in under 0.6 s) now runs on free-threaded builds too and passes.
- Server CPU per request rose in the measured rows (98.7 against 87.6 us at one I/O thread) while throughput rose; not explained, a hypothesis is that the larger pool's threads waste cycles when they compete.

## Verification

Python suite on 3.14t: 855 passed, 4 skipped (sandbox, 2 cores). The measured rows used a ceiling of 8; the new default's ceiling is 64 on that machine. Not verified: that the larger ceiling keeps the gain. The "default" rows of `benchmarks/suite.py --only cpu` on the owner's machine now run the new default and must be compared with the pool=2 rows above (68.2k and 66.1k). If they fall back to 58k and 64k, the ceiling is the cause and the decision must be revisited. Not measured: two and eight I/O threads, blocking handlers on the owner's machine, Python 3.13t/3.15t.
