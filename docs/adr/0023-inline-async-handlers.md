# ADR-0023: `async def` handlers that cannot suspend run on the I/O thread

## Status
Accepted (2026-10-26). The owner asked for the thread hop of `async def` to be removed and delegated the design (ADR-0022).
Broader scope (handlers that do await) is deliberately not done; see "Not done".

## Context
An `async def` request is parsed on the I/O thread, queued to the single asyncio loop thread, run there, and its answer is
posted back (ADR-0022, "Why `async def` is about 10 to 15 percent behind `def`"). Measured floor: the hop is about 4 us of 22 us.

## What was measured before deciding (2026-10-26, 2 cores shared with `oha`, Python 3.13)
1. **Ceiling.** A prototype that took the GIL on the I/O thread and ran a no-await handler there: 59.3k against 45.1k req/s
   (median of 5 interleaved rounds), 15.0 against 23.4 us of server CPU per request. Faster than `def` (49.1k).
2. **The price.** The same prototype in a mixed app (a CPU-bound `def` route running, plus `async def` and a static file
   at the same time): the static file, which needs no Python at all, went from p50 2.1 ms / p99 8 ms to p50 22 ms / p99 48 ms
   (and finished 364 of 2000 requests instead of 2000), because the I/O thread waited for the GIL. This breaks the rule of
   ADR-0009 and the `route_io` contract: the I/O thread never waits. So that design is rejected.
3. **A second problem, from asyncio itself.** User code written for asyncio assumes that code between two awaits is atomic
   with respect to other handlers (a plain `counter += 1`, a check-then-set on a dict). Running a handler on another thread
   while the loop thread runs callbacks breaks that, and so does touching the loop (`create_task`, `call_later`) from a thread
   that is not the loop's.

## Decision
A handler runs on the I/O thread only if all of this holds, and otherwise goes the old way (inbox, loop thread):
- **The handler cannot suspend.** Its own bytecode has no `await`, `async with` or `async for` (`_cannot_suspend`), it has no
  `Depends` (the dependency solver awaits), and Python is 3.12 or newer (eager tasks). Chosen at registration; the route is then
  registered with `route_io` through `_dispatch_hybrid`.
- **The loop thread is asleep.** The loop uses `_GatedSelector`: the loop thread owns a gate (a mutex) while it runs callbacks
  and gives it up only inside `select()`. The I/O thread only `try_lock`s it. Holding the gate is the same as being the loop thread
  between two iterations, so the atomicity rule above stays true. `loop_waits` stops the I/O thread from taking the gate again
  and again while the loop thread wants it back (no starvation).
- **No `def` handler, hook or stream callback is running** on a C++ thread (an atomic `busy` count, entered in every trampoline that
  takes the GIL). So the I/O thread does not wait for a GIL held by the pool.

Inside the inline step the loop looks like it is running (`_set_running_loop`), the handler runs in an eager `Task` (so
`current_task()`, `create_task`, timers behave as usual) with a copy of the loop thread's context, and if the handler scheduled
anything or suspended in a way we did not foresee (an exception handler that awaits, an after hook), the loop is woken through
its wake socket. Debug mode is refused (it checks the thread of every call).

C++ needs nothing new: `route_io` already is the inline path, and the gate and `busy` are about the GIL and the asyncio bridge,
the kind of difference ADR-0022 allows.

## Measured result (2026-10-26, same box, interleaved A/B, 6 rounds of 5 s, median)
| handler | before | after |
|---|---|---|
| `async def` with no await (`/async`) | 54.7k req/s, 19.5 us | 71.5k req/s, 12.9 us (+31 percent) |
| `async def` that awaits `asyncio.sleep(0)` | 47.6k, 24.0 us | 46.7k, 23.8 us (same, inside the noise) |
| mixed app, static file p50 / p99 while `def` CPU work and `async def` run | 2.0 ms / 6 ms | 1.9 ms / 6 ms |

## Not done, and why
- **Handlers that await** (nearly all real ones) still pay the hop. Running their first segment inline is possible with the same
  gate, but a thread-local used across an `await` would then see two threads, and the gain is only about half (the answer still has
  to travel back). Reopen with a measurement of a real handler (a database call).
- **Loop and I/O on one thread** (the loop's selector driven by Asio). It would remove the hop for every handler, but needs the
  loop's file descriptors inside Asio, differently on Windows. Too large for a measured gain we do not have yet.
- A Python thread that httpp does not know (a user thread, an executor) can still hold the GIL when the I/O thread takes it. The wait is
  bounded by the switch interval (5 ms). Not covered.

## Not verified
Windows and macOS. Free-threaded Python 3.14t (GIL off) was run on 2026-10-26: the whole Python suite (618 passed, 3 skipped, the inline tests included) and `benchmarks/stress.py` (20 s, 8 threads: 57.9k good answers, 0 wrong, no traceback) are clean. On 3.14t a no-await `async def` (59.0k req/s) and a `def` (59.7k) are equal, as expected without a GIL; an awaiting `async def` gets 49.4k. TSan covers the C++ core only; the gate is in the Cython layer and is not covered by it.

## Related
ADR-0009, ADR-0021, ADR-0022

## Addendum: fewer allocations and frees on the other thread (2026-10-26)
The handlers that await still pay the hop, so the cost of the hop was looked at again.
- **Allocations.** A malloc counter (one keep-alive connection, 20000 requests) gave 21.1 per `route` request but about 30 per `route_async` /
  `route_io` request. `responder::impl` had three `std::function` members and a wrapper closure around each way out; it now holds the way out,
  the way to close, and the 504 backstop entry as plain members. That gives 22 to 23 per request.
- **Where the memory is freed.** The per-request state is made on the I/O thread but was freed by whoever dropped the last reference, usually the
  asyncio loop thread. `wire_response::keep` now carries the responder state to the I/O thread with the answer, so it is freed there.
- **Bound (not shipped).** The same awaiting handler run with `LD_PRELOAD=libjemalloc.so.2` gave 58.0k against 50.6k req/s (+15 percent), but a `def`
  route gained 3.9 percent and the inline `async def` 4.9 percent from jemalloc alone, so only about 10 percent of that is the cross-thread free.
  httpp cannot ship an allocator (zero system dependencies).
- **Result** (interleaved, awaiting `async def`, old wheel against new, 7 and 9 rounds of 5 s): 43.4k to 45.2k and 44.4k to 46.3k req/s (+4 percent),
  CPU per request 25.7 to 23.5 and 25.1 to 23.2 us (-8 percent). The two runs agree, but the throughput gain is under the 5 percent noise line;
  the CPU number is the steadier one. Kept because it also removes code (no closure around each way out).
- **Not done.** The answer body is allocated on the loop thread and freed on the I/O thread, the mirror case. A body that fits the small-string
  buffer (15 bytes) costs nothing, a bigger one does. Not measured separately.
