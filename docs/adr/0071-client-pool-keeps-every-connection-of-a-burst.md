# ADR-0071: The client pool keeps every connection of a burst

- **Status:** accepted
- **Decided by:** the project owner (asked for `httpp.Client` on a free-threaded Python to scale like the GIL build); the cause and the fix are Claude's
- **Source:** `src/core/async_http.cpp` (`max_idle_per_origin`, `put_idle`), `tests/cpp/test_async_pool_proxy.cpp`, `benchmarks/diag_sync_client.py`

## Context

The engine keeps idle connections per origin for reuse, and it kept at most 8. Any connection finishing a request while 8 were already idle was closed. With a GIL, Python threads
reach the engine one at a time, so about 8 requests or fewer are in flight and the pool was enough. On a free-threaded Python (3.14t) 16 or 32 threads send requests at once, so every
burst above 8 closed its extra connections and the next burst opened them again (socket, connect, epoll registration, close).

Measured on Python 3.14t, 8 load CPUs, `httpp.Client` with one thread per Client, GET against the C++ benchmark server (`benchmarks/diag_sync_client.py`):

| threads | 8 | 16 | 32 |
|---|---|---|---|
| req/s | 147,517 | 55,032 | 26,908 |
| network thread CPU per request | 6.8 us | 18.7 us | 37.8 us |

The same run with the GIL: 110,597, 103,949, 101,715 req/s and 7.7 to 8.3 us. Turning the garbage collector off changed nothing (26,936 req/s at 32 threads). A `strace` of the
`httpp.Client` with 32 threads (Python 3.14t, one CPU, local Python server, 3 s run plus 1 s warmup) made 14,921 `connect` calls with the limit of 8 and 32 (one per thread) with the limit of 256: 8,626 and 15,269 req/s, network thread CPU 48.3 and 24.3 us per request, p99 7.8 and 4.0 ms.

## Decision

`max_idle_per_origin` is 256 instead of 8. A new connection is opened only when no idle one is available, so the pool never holds more than the peak concurrency; the limit only
bounds a huge burst, and the 60 second idle timer still closes what is no longer used.

## Rejected alternatives

A limit tied to `async_client::max_connections` (the blocking `client` has no such number, and the pool is shared by every client of the process); evicting idle connections after each
burst (a second burst a moment later would pay the handshake again, which is what this fixes); an unlimited pool (a burst of thousands would keep thousands of sockets).

## Consequences

- After a burst of N requests in flight (N up to 256), N idle sockets stay open per origin until the idle timeout (60 s). Before, at most 8.
- Test: `async_pool.a_burst_larger_than_eight_keeps_all_its_connections_for_the_next_burst` (24 requests held in flight together twice; the server must see 24 connections, not 40).
- **Not verified:** the numbers after the fix on the owner's machine (to be measured with `benchmarks/diag_sync_client.py` and `run.sh`), Windows, macOS.

## Related

ADR-0051 (the blocking client on the async engine), ADR-0059.
