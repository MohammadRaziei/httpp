# ADR-0059: The worker pool of a free-threaded Python starts at two threads

- **Status:** accepted (measured in the final configuration, with caveats under Verification); amends ADR-0025
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

## Second measurement: the ceiling and the growth wait (same machine, Release, 3.14.7t, 3 rounds, a download was running, so sd is up to 5 percent)

The first default (pool 2, ceiling 64, grow after 0.5 ms) gave no gain at all (59.5k and 65.4k req/s): under load the pool grew to many threads again. The rows of ADR-0025 had a ceiling of 8. Grid:

| pool 2 with | 1 I/O thread | 4 I/O threads | p99 ms (1 / 4) |
|---|---|---|---|
| old default (16 threads from the start) | 57.4k | 62.4k | 2.49 / 3.33 |
| ceiling 8, grow 0.5 ms | 66.2k | 62.6k | 2.12 / 3.53 |
| ceiling 16, grow 0.5 ms | 56.0k | 60.2k | 2.54 / 3.25 |
| ceiling 64, grow 10 ms | 26.4k | 25.7k | 3.30 / 3.34 |
| **ceiling 64, grow 2 ms** | **68.8k** (63.9 to 69.7) | **63.2k** (60.7 to 68.3) | **1.33 / 1.44** |

A growth wait of 10 ms is rejected: the pool stays at two threads and CPU-bound `def` falls to the level of `async def`. A wait of 0.5 ms with any ceiling above 8 grows the pool back to the slow size.

## Decision

On a free-threaded Python the default pool is `set_thread_pool(2, max(8, CPUs) * 4, grow_after_ms=2)`: two threads at the start, the C++ default's ceiling, growth after a 2 ms wait. `threads=` given by the caller is unchanged. Builds with a GIL are unchanged.

## Consequences

- A handler that blocks (sleep, I/O) on a free-threaded build now waits up to 2 ms before the pool adds threads (four per step of the wait, ADR-0025) instead of finding them idle. Test `test_blocking_handlers_still_run_together_with_the_small_default_pool` (24 handlers of 0.2 s in under 0.6 s) now runs on free-threaded builds too and passes.
- Server CPU per request fell as well (80 against 96 us at four I/O threads, one round pair) and p99 halved; the cause is not investigated (hypothesis: fewer threads compete for the same queue).

## Verification

Python suite on 3.14t in the sandbox (2 cores). Throughput numbers from the owner's machine with a download running in the background (sd 0.5 to 6.9 percent). Not verified: two and eight I/O threads, blocking handlers on the owner's machine (only the sandbox test), Python 3.13t and 3.15t, a run without background load.
