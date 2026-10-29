# Hand-off 11

Read this first in the next session. It replaces handoff_10.md (older hand-offs stay for history).

## Working rules (from the owner)
- Talk to him in Persian. Everything inside the repo (docs, ADRs, comments, tests, changelog) is English, no em dash.
- Never push. After every step build `httpp.zip` (without `__pycache__`, `build`, `.git`, `.venvs`, `.build`, `bin`) and send it.
- He decides nothing technical: only optimality, speed and a correct architecture matter. Tell him honestly what was measured and what was not.
- TDD, ponytail style, zero system dependencies, STL-like snake_case C++, one design for the C++ and the Python API (only GIL related differences).
- Numbers are only comparable inside one run (interleaved rounds). The sandbox has 2 cores shared with the load generator: it cannot show
  multi-core scaling. His machine can: Ubuntu 26.04, i7-13620H, 16 threads (P-cores 0-11, E-cores 12-15), powersave governor.

## State
- C++: 295 tests pass, TSan clean on the engine. Python: 625 tests pass on 3.13 and 3.14 (GIL), 624 (+1 skip) on 3.14t.
  Build only `--target test_httpp_cpp` (the default target fails in the nested Python build). Venvs under `/home/claude/work`:
  venv13, venv314, venv314t, venv_old (baseline). Rebuild a wheel with `uv pip install --python ../venvX/bin/python --no-build-isolation --reinstall ./`.
- Done in this stretch (ADRs 0023 to 0025):
  - ADR-0023: `async def` handlers that cannot suspend can run on the I/O thread (gate, busy count).
  - ADR-0024: `server::set_io_threads(n)` (one engine per I/O thread on one port, SO_REUSEPORT), Python lanes (an asyncio loop per I/O thread only on a free-threaded Python), `Server(io_threads=)`, `httpp run --io-threads`.
  - ADR-0025: defaults from the 16-thread measurement. `io_threads` default is `min(4, CPUs // 4)` with a GIL, 1 without a GIL or on Windows. `inline_async` default is on only with 2 CPUs or fewer (it loses 20 percent at saturation when cores are free). With a GIL the worker pool starts with 1 thread and grows after a 10 ms wait; `set_thread_pool(base, max, grow_after)` is new in C++ and Python (`grow_after_ms`). A growth step adds one thread per 500 us of the wait.
- `benchmarks/Makefile`: `make bench` (long, about 40 minutes; `make quick`, about 3) builds venvs 3.14 (GIL) and 3.14t, `oha`, the C++ server, and writes `benchmarks/results/<time>.txt`.
  The suite holds hello, json, cpu, one-connection latency, fixed-rate latency (p99.9 at half of each maximum), uvicorn, FastAPI, httpp def / async / await / inline, C++ at 1, 2, 4, 8 I/O threads.

## Measured on the owner's machine (3.14 GIL / 3.14t, 64 connections, load generator on 3 CPUs, so rows above about 300k are capped by it)
- hello: httpp `def` 1 / 2 / 4 I/O threads: 170k / 315k / 366k req/s; uvicorn bare ASGI 65k; C++ `route_io` 170k / 324k / 392k.
- `async def` (no inline) about 94 percent of `def` at 1 thread; inline 120k (loses); with await 146k.
- CPU-bound (about 60 us) `def` with a GIL: 19.9k req/s, p99 3.8 ms with the new pool default (it was 10.9k, 26 ms); fixed pool 1: 15.8k; 16: 11.8k. uvicorn 15.6k.
- CPU-bound without a GIL: `def` 46k to 72k (before the last pool change it was 60k to 68k: the doubling step cost 20 percent at one I/O thread; it was reverted, not yet re-measured); `async def` scales with lanes 18k / 35k / 61k for 1 / 2 / 4.
- p99 at 4+ I/O threads is 3 to 6 ms in C++ too (p50 0.05 ms): probably the load generator or the scheduler, not httpp. The fixed-rate phase of the suite is there to settle it.

## Bench read (ADR-0025 addenda 4 and 5)
The first run had a bad CPU split (shared hyper-thread pair, 3 CPUs for `oha`); its C++ ceiling and p99 tails were artifacts. The second run (core-aligned split, tree f08eea4db0) is the reference: see addendum 5. Open: free-threaded pool base 2 (+17 percent at 1 I/O thread, not yet at 4), `def JSON` plateau on 3.14t, non-monotone inline on 3.14t.

## Crash found (ADR-0025 addendum 6)
Two bench rounds lost the server (Python `def` GIL io=8, C++ `route` io=4). TSan on the C++ bench server found a data race in `http_date()` (shared static cache across I/O threads); fixed with `thread_local`, regression test added. Not proven that it is THE cause: rerun `make bench` and look for error rows. The sandbox has no way to run the 16-thread bench but can run TSan/ASan builds of the core under `oha` (see `/home/claude/work/b_thread`, `cpp_bench_thread`, recipe: CMAKE_CXX_FLAGS=-fsanitize=thread, build `httpp_core`, compile `benchmarks/cpp/cpp_bench.cpp` with the same flag). The earlier statement that TSan was clean covered tests with one I/O thread only.

## Next
1. Ask for `make bench` with the governor on `performance` and the new pool rows at 4 I/O threads. Read the next output (ask the owner to run it with the governor on `performance`): check that 3.14t `def` CPU-bound is back at about 60k, read the fixed-rate p99.9 table, check FastAPI and JSON rows, and the rows at 8 I/O threads. Record the numbers in an ADR-0025 addendum and fix defaults if the data says so.
2. WebSocket (RFC 6455) in the C++ core first, then the Python binding. ADR and tests first: handshake, frames (text, binary, continuation), masking, ping/pong, close codes, size limits, fragmentation, per-message limits, backpressure. It must run on the I/O thread like `route_io`, and the Python side follows the lanes design.
3. Small items: README gaps (HTTPException, response classes, status, 405), `listen()` via `signal.set_wakeup_fd`, thread-safety review of module level Python state under free-threading (lanes make it matter), FastAPI benchmark with `response_model` and dependencies.
4. CI: last failures were `test_the_command_line_takes_io_threads` (the child process could not import httpp: fixed by pointing PYTHONPATH at the tested copy like `test_run.py` does) and the macOS spread assertion (macOS does not spread connections over SO_REUSEPORT sockets: now Linux only). Check that CI is green.
5. Optional: callgrind of the C++ I/O path; a memory and sustained load test (the owner's reviewer asked for it).

## Not verified
Windows and macOS runs (macOS wheel tests had one failure, fixed, not re-run). Many-core scaling above what the load generator can drive. Behaviour on battery or with the performance governor.
