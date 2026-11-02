# ADR-0033: Fewer instructions per request in the engine (no reactor locking, no hop, lazy timers, cheaper head)

- **Status:** accepted
- **Source:** `src/core/async_server.cpp` (`io_context` hint, `t_loop_engine`), `benchmarks/cpp/cpp_bench.cpp` (third argument)

## Context

In the full benchmark (2026-11-01, 16 threads) the C++ `route_io` answered `hello` at 208k req/s with one I/O thread against 262k for Drogon,
and used 4.8 us of CPU per request against 3.8. Drogon also runs an epoll reactor (its own library, trantor, not libuv), so the loop is not the
difference. The sandbox has no `perf`, so instructions were counted with `valgrind --tool=callgrind` while `oha` sent 2000 and 22000 requests at
16 connections; the difference divided by 20000 is the cost of one request (startup cancels out). Under valgrind this is about 25.5k
instructions per request for httpp and 14.6k for Drogon: the absolute numbers are inflated (one request per `epoll_wait`), the ratio is the point.

Findings: `llhttp` is not the gap (1.5k instructions per request, Drogon's parser and `addHeader` cost more). The gap is spread over our own layers:
29 percent of all instructions ran inside `poster::post` (a mutex, a `std::function` holding a `wire_response`, an `asio::post`, a later move and
destruction) even when the answer was made on the loop thread itself, which is every `route_io` answer; 1.2k instructions per request in pthread
mutexes (Drogon: none in its top 20); 420 per request in `epoll_reactor::cancel_timer`; and about 3k in moving `response`, `wire_response` and
header vectors between layers.

## Decision

1. `asio::io_context io{ASIO_CONCURRENCY_HINT_UNSAFE_IO}`: the reactor takes no locks for descriptor registration and I/O. This needs every socket and
   timer to be used on the loop thread only, which the engine already does (its comments say so; other threads call `poster::post`). The
   scheduler keeps its lock, so `asio::post` from any thread is still safe. Measured: 25.5k to 24.7k instructions per request (3 percent).
2. An answer made on the engine's own loop thread is sent at once. `answer_fn` compares a `thread_local` pointer (`t_loop_engine`, set around
   `io.run()`) with its engine; on a match it calls `conn::send` directly, otherwise it goes through `poster` as before. The pointer is only compared,
   never dereferenced, so a late answer after the engine is gone is still safe. Measured: 24.7k to 23.2k (6 percent more). Total 9 percent.
3. Connection timers are lazy. `arm` and `disarm` only write a deadline (one clock read); the Asio timer is scheduled when there is none or when the
   new deadline is earlier than the one it waits for, and when it wakes early it waits again for the current deadline. A keep-alive request no longer
   touches the timer queue (before: two `expires_after` and two cancels per request). Measured: 23.2k to 21.2k (9 percent more).
4. The response head is built with appends into one reserved string, and the four headers the engine owns are recognised by length first (before: three
   temporaries per header and four case-insensitive compares). Measured: 21.2k to 18.7k (12 percent more). Total: 25.5k to 18.3k, 27 percent.
5. **Changed API (dual, C++ and Python):** `keep_alive_max` is `1000` by default (was 100) and `0` means no limit (it meant 1, which nobody wants). Drogon's default
   is no limit, nginx's is 1000. Test: `keep_alive_max_zero_means_no_limit`, and a `0` case in the Python keep-alive test.
6. Small copies removed: `to_wire` moves the headers instead of copying them, `make_request` moves the target, `match` decodes only path parts that contain `%`, and
   `ieq` no longer calls `tolower`. Measured: 18.7k to 18.3k (2 percent).
7. **The 504 backstop list is per I/O thread** (`server::impl::waiting`). Every async request (`route_io`, `route_async`, Python `async def`, a WebSocket handshake) was
   entered in ONE list under ONE mutex shared by all I/O threads, and removed under the same mutex when it was answered: two lock round trips per request on a
   shared cache line. A `perf record` of `cpp_bench` with 4 I/O threads on the owner's machine (2026-11-02) showed `pthread_mutex_lock/unlock` at 1.9 percent of
   all cycles and `track` at 1.2; the benchmark showed `route_io` with 2 threads (394k req/s) slower than `route`, which does not use the list (433k). Now each
   engine has its own list and sweep timer, entered from its own I/O thread; threads that are not I/O threads (pool workers) still use one shared list. Test:
   `unanswered_io_requests_get_a_504_on_every_io_thread_and_answered_ones_are_not_touched`. Instruction counts per request do not change (one thread); the gain
   is not measured yet.
8. `cpp_bench` takes the keep-alive limit as a third argument, for A/B runs.
9. **No `weak_ptr<config>` when nothing needs the config.** `make_sender` took a `weak_ptr<config>` and locked it for every answer: the copy, the lock and the two releases are
   atomic operations on one control block that all I/O threads share. The config is read-only once listening, and it is used only by the `error` and `after` hooks and by
   compression. When none of them is set (the plain case), the sender now holds only the request and the engine's answer function, and builds the wire response itself
   (`to_wire` became static). A late answer after the server is gone stays safe because the engine's answer function holds only weak and shared engine state. When a hook is set the
   old sender is used unchanged. Why not a per-engine token: it would keep a shared count per engine and still need the lock; dropping the need is simpler and removes the cost.
   Verified: the C++ suite (386 tests) passes. **Not measured:** the gain (the effect is cache-line contention, which callgrind does not count, and the sandbox has no `perf`); the
   per-request `shared_ptr` copies of `responder::impl`, the `once` flag and the engine's `poster` remain and are per engine or per request. TSan was not re-run for this change.

## Rejected alternatives

- `ASIO_CONCURRENCY_HINT_UNSAFE` (scheduler lock off too): other threads post answers, so it would be a data race.
- A timing wheel shared by all connections (as Drogon has): the lazy deadline already removes the per-request timer work; the wheel only saves the one
  Asio timer per connection and is a bigger change.

## Consequences

- Verified: the whole C++ suite (385 test cases) passes under ThreadSanitizer with no warning; the Python suite on 3.14 (GIL) gives 653 passed, 4 skipped.
- **Not verified:** wall-clock gain on the owner's machine (instruction counts are not time; on the 2 core sandbox, one core each, `route_io` already
  matched Drogon within noise, so the 25 percent gap seen on 16 threads could not be reproduced there), Windows, macOS, free-threaded Python, the
  Python layer under TSan. Any future code that touches a socket or timer from another thread breaks the hint's rule; TSan will usually show it.
- The old default of 100 requests per connection made every httpp row of the benchmark reconnect every 100 requests while Drogon did not. In an
  interleaved sandbox A/B of `route_io` the unlimited setting gave about 7 percent more req/s (three rounds, noisy). Both servers now keep connections alive.
- The same `perf` profile shows shared_ptr reference counting at about 6 percent of all cycles (`_M_release` 3.1, copy 1.8, weak release 0.6, `weak_ptr::lock` 0.5); the
  `weak_ptr<config>` that `make_sender` locks for every answer is one counter shared by all I/O threads. Not changed yet.
- Remaining per-request cost (about 18.3k against 14.6k for Drogon under the same counting): the response moves between layers (`response`, `wire_response`,
  header pair vectors), the `request` built from `http_request`, `make_shared` of the `once` flag and of the answer closure, and the parser callbacks that append to strings.

## Related

ADR-0023 (inline async handlers), ADR-0024 (lanes), ADR-0030 (benchmarks built by CMake).
