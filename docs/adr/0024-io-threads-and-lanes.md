# ADR-0024: several I/O threads, and one asyncio loop per I/O thread on a free-threaded Python

## Status
Accepted (2026-10-27). The owner asked for the optimizations that remove the speed ceiling of one I/O thread and one loop thread,
and delegated the design. Measurements on the target machine (16 threads) are still to come from `benchmarks/Makefile`
(see "Not verified"); the default stays at one I/O thread until they exist.

## Context
One engine runs on one I/O thread: it accepts, reads, parses and answers every connection. On a machine with many cores this is the
ceiling for anything cheap (the C++ `route_io` hello answer costs about 15 us of CPU, so one thread tops out near 60k req/s).
Python adds a second ceiling: all `async def` handlers run on one asyncio loop thread. With a GIL a second loop buys nothing,
because only one thread runs Python at a time. On a free-threaded Python a second loop does run in parallel.

## Decision
1. **C++: `server::set_io_threads(n)`.** One `async_server` engine per I/O thread, each with its own acceptor on the same port
   (`SO_REUSEPORT`; the kernel spreads new connections). A connection stays on its engine for good, so the requests of one connection
   never overlap and `route_io` still never needs a lock. Every setter is replayed on every engine (`configure`), `set_max_connections`
   is a total shared out evenly. Engine 0 runs on the thread that calls `listen`; the others run on threads the server starts and
   joins. Where `SO_REUSEPORT` is missing (Windows) or a bind fails, the server keeps one thread and logs a warning.
   A `thread_local` flag marks the I/O threads (it replaces the single `io_thread` id).
2. **Python: lanes.** A lane is one I/O thread plus one asyncio loop thread, with its own inbox, gate, wake socket and deadline table
   (ADR-0023 per lane). An I/O thread is given a lane the first time it dispatches a request. `Server(io_threads=N)`,
   `Server.set_io_threads(N)` and `httpp run --io-threads N` set the count.
3. **Lanes follow the I/O threads only without a GIL.** With a GIL there is one lane, whatever `io_threads` is: the extra I/O threads
   still parse and answer in parallel (they do not need the GIL), and all Python runs on the one loop. Without a GIL there are
   `io_threads` lanes. There the "no `def` handler is running" half of the gate (the `busy` counter of ADR-0023) is skipped,
   because Python code no longer excludes other Python code; the gate of the lane stays, so asyncio's atomicity rule still holds
   inside a lane. Code that shares state between lanes is shared-state code like any other threaded code.
4. The default is 1 (nothing changes unless asked).

## Rejected
- **One loop shared by all I/O threads**: the loop thread stays the ceiling, which is the thing being removed.
- **Several loops with a GIL**: they only fight for the GIL (measured earlier: no gain, more switches).
- **Loop and I/O on one thread** (like uvicorn): loses isolation (a slow handler stalls parsing) and is a different design per platform.
- **Spreading one connection over lanes**: breaks per-connection ordering for no gain.
- **A default above 1**: not decided without numbers from a machine with enough cores.

## Consequences
- `route_io` handlers must be safe to run on several threads at once when `io_threads > 1` (they already had to be safe against
  the worker pool).
- Memory and file descriptors grow with the count (one acceptor, one timer set per engine).
- Lane state of the process (`_lanes`) is shared by every `Server` in the process.

## Not verified
- Scaling on many cores. The sandbox has 2 cores shared with the load generator. Use `benchmarks/Makefile` (`make bench`).
- Windows and macOS (macOS has `SO_REUSEPORT` but does not spread connections like Linux; not run).
- The best default for `io_threads`.

## Related
ADR-0009 (hot path), ADR-0022, ADR-0023 (inline `async def`).
