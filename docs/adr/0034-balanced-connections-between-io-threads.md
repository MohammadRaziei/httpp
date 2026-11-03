# ADR-0034: Connections are shared evenly between I/O threads

- **Status:** accepted
- **Source:** `src/core/async_server.cpp` (`balance_with`, `pick_engine`, `adopt`), `src/core/server.cpp` (`listen_after_bind`), `benchmarks/diag_io.sh`

## Context

With `set_io_threads(n)` every I/O thread has its own acceptor on the same port (`SO_REUSEPORT`, ADR-0024) and the kernel decides which acceptor gets a new
connection by a hash of the addresses and ports. For a few dozen connections that hash is uneven. Measured here (strace on `accept4`, `cpp_bench` with 4 I/O threads,
64 connections, six trials): per thread 11/10/8/8, 17/15/9/9, 13/12/8/6, 11/10/10/8, 12/10/5/3, 11/10/10/10 (the counts are low because some `accept4` lines
were unfinished in the trace; the shape is the point). On the owner's machine (2026-11-03, `diag_io.sh`, closed loop, 64 connections, 4 I/O threads) the
CPU ticks of the four server threads over 3 seconds were 306, 301, 257, 251 for httpp and 301, 301, 301, 296 for Drogon, which gives every connection to a loop in
turn. In a closed loop a thread with fewer connections waits for its clients while the one with more is saturated, so part of the capacity stays unused. httpp
reached 659k to 705k req/s with 4 threads and Drogon 796k to 878k. An earlier reading of the first, buggy tick output as "evenly spread" was wrong.

The same profile showed that both servers spend about 63 to 65 percent of their cycles in the kernel, in the same functions with the same shares, so the
network path itself is not the difference.

## Decision

Engines that share a port know each other (`async_server::balance_with`, called from `server::listen_after_bind` with every engine). The engine whose acceptor wakes up
asks every engine for its `load` (open connections plus connections already promised to it), takes the smallest (itself on a tie), adds one to that engine's `load`
at once (so a burst of accepts does not all pick the engine that looks empty) and, if it is another engine, hands the descriptor over: `release()` from this reactor,
one `poster` hop to the target's loop thread, `assign()` into the target's `io_context`, then the usual connection setup. One hop per new connection, never per
request. A descriptor whose target is already shutting down is closed by a small owner object, not leaked. The per-engine connection limit
(`set_max_connections`) is checked where the connection is adopted, so it applies to the engine that will really own it.

With one engine the list of peers is empty and nothing changes except one atomic add per connection.

## Rejected alternatives

- One acceptor on engine 0 that deals connections out in turn (what Drogon does): serial accept on one thread, and it throws away `SO_REUSEPORT`, which also lets several
  processes share a port (`run(workers=N)`). Counting load handles uneven connection lifetimes too (a thread that lost its connections gets the next ones).
- `SO_INCOMING_CPU` or an eBPF reuseport program: Linux only, privileges or a kernel version, and still not exact.
- Doing nothing and calling it noise: the tick counts differ by 20 percent in the same run, repeatably.

## Consequences

- Verified: two new engine tests (4 engines, 64 connections opened one after another: exactly 16 each; a handed-over connection serves requests and frees its slot), the whole C++ suite
  (391 tests) passes, also under ThreadSanitizer with no warning.
- **Not verified:** the throughput gain. It is expected to be a few percent (two of four threads were about 16 percent idle) and it cannot close the whole gap to Drogon (about 17 percent
  in the same runs, 4.55 us of CPU per request against 5.5). Windows and macOS (the descriptor handover uses `release` and `assign`, which Asio supports there, but nothing ran). The Python layer.
- Connection-per-request clients pay one extra hop per connection when the accepting engine is not the least loaded one.
- Still open: why the kernel time per request is about 15 percent higher for httpp with the same functions (`benchmarks/diag_io.sh`, part 3 counts syscalls, context switches, migrations,
  IPC and cache misses per request for both servers).

## Related

ADR-0024 (I/O threads and lanes), ADR-0033 (per-request cost).
