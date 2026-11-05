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

10. **Instruction counts, round two (2026-11-02, `cpp_bench` `/io`, same callgrind recipe, 8 keep-alive connections, `keep_alive_max` 0).** A `perf record` (cycles, user space) of
    `route_io` with 4 I/O threads showed: shared_ptr and weak_ptr counting about 10.7 percent, malloc and free about 7, `strlen` 3.7 plus `ieq` 1.2, mutex lock and unlock 3.6 (`track`
    1.1), `llhttp` 6.5. Per request, before and after each step (the steps are cumulative): 15425 at the start (item 9 already in), 14497 head written with copies of constant
    size into one buffer of a known upper bound (no `strlen`, no append per piece), 14168 `ieq` with an inline length test and `header_ok` on views, 13590 `response` passed by
    rvalue reference through `send_fn` and `to_wire`, 13345 and 13213 the same for `answer_fn` and `conn::send` (`wire_response&&`), 12381 one shared `answer_state` for the answer
    function (one allocation, one count; it fits the `std::function` storage) and no deadline entry for a request that is already answered when its handler returns, 11869 `to_wire`
    takes the handler's header vector as it is. Total 15425 to about 11.8k (23 percent). `/sync` (worker pool) is 14.1k now; it was not counted before, so there is no before.
11. **The 504 deadline starts when the handler returns** (it started at arrival). A handler that answers on the spot (the usual `route_io` case) is never entered in the list: no lock, no
    node, no count. The difference is the handler's own run time, which a `route_io` handler keeps near zero. Because `track` now runs while another thread may already hold the
    responder, `responder::impl::unwait` is an atomic function pointer stored last with release and read with acquire (`wait` and `wait_id` are written before it). ThreadSanitizer
    found exactly this race in the first version of the change (a plain pointer, read by an answering thread); with the atomic the whole C++ suite is clean.
12. `to_wire` sends exactly one `Content-Type`, last, with the value of the last one the handler set, `text/plain` when none (unchanged); the vector the handler filled is
    reused. Three wire tests pin the behaviour (one in the middle, several, none).

## Rejected alternatives

- `ASIO_CONCURRENCY_HINT_UNSAFE` (scheduler lock off too): other threads post answers, so it would be a data race.
- A timing wheel shared by all connections (as Drogon has): the lazy deadline already removes the per-request timer work; the wheel only saves the one
  Asio timer per connection and is a bigger change.

## Consequences

- Verified (items 1 to 8): the whole C++ suite (385 test cases) passes under ThreadSanitizer with no warning; the Python suite on 3.14 (GIL) gives 653 passed, 4 skipped.
- **Not verified:** wall-clock gain on the owner's machine (instruction counts are not time; on the 2 core sandbox, one core each, `route_io` already
  matched Drogon within noise, so the 25 percent gap seen on 16 threads could not be reproduced there), Windows, macOS, free-threaded Python, the
  Python layer under TSan. Any future code that touches a socket or timer from another thread breaks the hint's rule; TSan will usually show it.
- The old default of 100 requests per connection made every httpp row of the benchmark reconnect every 100 requests while Drogon did not. In an
  interleaved sandbox A/B of `route_io` the unlimited setting gave about 7 percent more req/s (three rounds, noisy). Both servers now keep connections alive.
- The same `perf` profile shows shared_ptr reference counting at about 6 percent of all cycles (`_M_release` 3.1, copy 1.8, weak release 0.6, `weak_ptr::lock` 0.5); the
  `weak_ptr<config>` that `make_sender` locks for every answer is one counter shared by all I/O threads. Not changed yet.
- Remaining per-request cost (about 18.3k against 14.6k for Drogon under the same counting): the response moves between layers (`response`, `wire_response`,
  header pair vectors), the `request` built from `http_request`, `make_shared` of the `once` flag and of the answer closure, and the parser callbacks that append to strings.

- Verified (items 9 to 12): 389 C++ tests pass, also under ThreadSanitizer (whole suite, after the race of item 11 was fixed). **Not verified:** wall-clock gain (instruction counts only; the sandbox has
  no `perf`), the scaling gap with 4 I/O threads (route_io 659k against Drogon 812k on the owner's machine, us per request 3.8 / 4.0 / 5.7 for 1 / 2 / 4 threads against Drogon 3.7 / 3.5 / 4.8:
  the cost per request grows with threads, the cause is not found; a `perf` of that run shows no single hot spot in user space), the Python suite and Python 3.13 and 3.14t (core internals only,
  no public API changed, but not run), Windows, macOS.

## Related

ADR-0023 (inline async handlers), ADR-0024 (lanes), ADR-0030 (benchmarks built by CMake).

## Addendum (2026-11-05): literal comparisons with `string_view`

An owner `perf` run (4 I/O threads) put `malloc` plus `free` at about 2.9 percent of cycles, string code in libstdc++ at about 2.7, `_Sp_counted_base::_M_release` at 1.96 (self) and Asio's scheduler mutex at about 0.9.
Callgrind (1 I/O thread, `route_io`, per request): 13 `malloc` and 13 `free`, about 14 `_M_release`, 3.2 `pthread_mutex_lock` (all in `asio::scheduler::run`), 10 `strlen`, 4 `steady_clock::now`.
The allocations are spread over many call sites (3 from header pair copies, 2 in `run`, 2 in `request_complete`, the rest single); no one site dominates, and the header vectors are already moved.
Done: comparisons of the method and target with string literals (`make_request`, `serve_static` test, HEAD to GET mapping, engine `req_head`) go through `std::string_view`, so no `strlen` call: 10 to 7 `strlen` per request,
13440 to 13327 instructions per request (0.8 percent, callgrind, sandbox). Not measured on the owner's machine; expected to be within noise there. C++ suite: 407 passed.
Not done: fewer `shared_ptr` copies (about 14 releases per request spread over `send`, `run`, `request_complete`, `~wire_response`); the ceiling is about 2 percent of cycles and it touches request lifetime, so it needs ASan, TSan and the outlive tests.

### Owner's A/B runs, 2026-11-05 (`diag_io.sh`, 4 I/O threads, httpp and Drogon alternating, 3 + 3 rounds in two sessions, `keep_alive_max` 0 only in the second)

Requests per second, httpp: 694k, 693k, 647k, 570k, 657k, 641k (median 652k, range 570k to 694k). Drogon: 606k, 597k, 646k, 668k, 603k, 629k (median 618k, range 597k to 668k).
The ranges overlap, so this does not show that httpp is faster; it shows the 7 to 10 percent gap of the earlier single runs is gone in these runs. The first number of each pair is measured under `perf record`.
Counters of the last pair (one window, single run): cycles per request 23.2k (httpp) against 23.6k (Drogon), instructions 28.7k against 30.6k, cache misses 2.6 against 4.5, thread ticks 308 against 300 (httpp's four I/O threads are the busier ones).
The earlier pair had 29.8k against 27.1k instructions: Drogon's own figure moved by 3.5k between sessions, so cross-session comparisons of these counters are weak. Not measured: the interleaved official bench (the owner's `uv` could not reach PyPI), a `performance` governor, more than one machine.
