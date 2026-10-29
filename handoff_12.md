# Hand-off 12

Read this first in the next session. It replaces handoff_11.md (older hand-offs stay for history).

## Working rules (from the owner)
- Talk to him in Persian. Everything inside the repo (docs, ADRs, comments, tests, changelog) is English, no em dash.
- Never push. After every step build `httpp.zip` (without `__pycache__`, `build`, `.git`, `.venvs`, `.build`, `bin`) and send it.
- He decides nothing technical: only optimality, speed and a correct architecture matter. Tell him honestly what was measured and what was not,
  and say plainly when an earlier statement of yours was wrong (it happened twice this stretch: the CPU split and the "TSan clean" claim).
- TDD, ponytail style, zero system dependencies, STL-like snake_case C++, one design for the C++ and the Python API (only GIL related differences).
- Numbers are only comparable inside one run (interleaved rounds). The sandbox has 2 cores shared with the load generator: it cannot show
  multi-core scaling, but it CAN run sanitizer builds under load (below). His machine: Ubuntu 26.04, i7-13620H, 16 threads (P-cores 0-11 as 6 hyper-thread pairs, E-cores 12-15), powersave governor, project on a Windows drive mounted under /mnt/windowsD.

## State
- C++: 296 tests pass (the new one is `httpp_server_io_threads.the_date_header_is_well_formed_when_every_io_thread_answers_at_once`). Python: 625 tests pass on 3.13 and 3.14 (GIL), 624 (+1 skip) on 3.14t (not re-run after the Date fix, which is C++ only).
  Build only `--target test_httpp_cpp` (the default target fails in the nested Python build). The sandbox has been reset: the old venvs are gone. Recreate with `uv venv --python 3.14 X` and `uv pip install --python X/bin/python ./` (about 70 s).
- ADRs 0023 to 0025 (see them for the reasoning):
  - 0023: `async def` handlers that cannot suspend can run on the I/O thread (gate, busy count).
  - 0024: `server::set_io_threads(n)` (one engine per I/O thread on one port, SO_REUSEPORT), Python lanes (an asyncio loop per I/O thread only on a free-threaded Python), `Server(io_threads=)`, `httpp run --io-threads`.
  - 0025: defaults. `io_threads`: `min(4, CPUs // 4)` with a GIL, 1 without a GIL or on Windows. `inline_async`: on only with 2 CPUs or fewer. With a GIL the worker pool starts with 1 thread and grows after a 10 ms wait
    (`set_thread_pool(base, max, grow_after)`, Python `grow_after_ms`; a step adds one thread per 500 us of the wait). Without a GIL the C++ default pool is used (base `max(8, CPUs)`).
    Addenda 4 to 6 hold the bench readings, their corrections, and the crash.
- Done in the last session (all in the zip): `-Wreorder` fixed in `worker_pool.hpp`; **data race fixed in `http_date()`** (`async_server.cpp`, cache is now `thread_local`; see below);
  `benchmarks/`: core-aligned CPU split (server 0-7, load generator 8-15 on the owner's machine), `tree` hash in the result header, fixed-pool rows for 3.14t (2, 4, 8 at 1 and 4 I/O threads), the suite prints exit code, error kinds and stderr tail when a run has errors,
  Makefile installs `jsonschema` and `jinja2` next to cmake and ninja (the mbedtls generator needs them; the venv Python is now passed to cmake), `UV_LINK_MODE=copy`, `-Wno-deprecated` (silences third-party notes only).
- Known and left alone: the `liburlparser` configure downloads the public suffix list and rewrites the tracked `src/third_party/liburlparser/src/public_suffix_list.dat` (network at configure, a modified file in `git status`). It is the owner's library and a design decision; say it again if it comes up.
- `make bench` (about 40 minutes): if a configure failed halfway, `rm -rf benchmarks/.build` before rerunning. `make quick` is about 3 minutes.

## Reference numbers (owner's machine, core-aligned split, 64 connections, medians of 3, tree f08eea4db0 and 9c83b41592; req/s)
Earlier numbers in handoff_10 and ADR-0025 addendum 4 came from a split that shared a hyper-thread pair and gave `oha` 3 CPUs: ignore them (C++ ceiling, p99 tails, the "GIL pool halves throughput" claim were artifacts).
- hello, py 3.14 GIL: `def` 211k / 348k / 502k / 423k for 1 / 2 / 4 / 8 I/O threads (peak at 4); `async def` 193k / 313k / 300k / 270k (one loop caps it); uvicorn 83k; FastAPI 22k.
- hello, py 3.14t: `def` 211k / 354k / 516k / 407k; `async def` 195k / 315k / 477k / 443k (scales with lanes); inline `async def` 145k / 267k / 351k / 529k (not monotone, unexplained).
- C++: `route` 206k / 376k / 513k / 695k; `route_io` 203k / 377k / 527k / 792k. So Python `def` reaches about 95 percent of C++ at 4 I/O threads; at 8 it falls behind (the server has 4 physical cores).
- CPU-bound (about 60 us), GIL: 24-25k at every thread count and every pool size; only p99 differs (default 3.0 ms, pool 4: 5.4, pool 16: 11.1). uvicorn 20k. 3.14t `def`: 65k / 68k / 68k / 70k; pool starting at 2: 76.7k at 1 I/O thread (equal at 4). `async def` with lanes 23k / 41k / 67k / 73k.
- JSON, 3.14t: `def` plateaus near 300k from 2 I/O threads, `async def` reaches 434k at 4. GIL: `def` 209k / 328k / 298k / 275k. FastAPI 17k.
- One connection: inline 60-66k req/s (p50 0.01 ms), `def` 44-53k, `async def` 40-43k, C++ `route_io` 74k.
- Fixed rate at half of each maximum: p99.9 0.8 to 1.3 ms (Python and C++), p99 about 0.5 to 0.75 ms.

## The crash (ADR-0025 addendum 6)
The full bench had two rounds that lost the server: py 3.14 `def` at 8 I/O threads (131k, 606,771 errors) and C++ `route` (pool) at 4 I/O threads (a round with 0 answers, 2.5M errors), out of about 150 server starts.
A TSan build of the C++ bench server under `oha` showed 5 data-race warnings per run, all in `http_date()` (function-level statics shared by every I/O thread). Fixed with `thread_local`; 0 warnings afterwards at 4 and 8 I/O threads, ASan clean in six runs.
**Not proven** that this is the cause of both lost servers (no core file). The next full bench decides: error rows gone, or the new report says how the server died. If one persists: get a core (`ulimit -c unlimited`) or run the Python server under `faulthandler`/gdb, and TSan the Python extension too (only the C++ core was done).
Recipe in the sandbox: configure with `-DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" -DCMAKE_C_FLAGS=... -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread -DHTTPP_BUILD_PYTHON=OFF -DHTTPP_BUILD_TESTS=OFF`, build `httpp_core`, compile `benchmarks/cpp/cpp_bench.cpp` with the same flag (`-L<build> -lhttpp_core -Wl,-rpath,<build>`), run `cpp_bench <port> <io_threads>` and hit `/sync` (pool) or `/io` (inline) with `oha -z 6s -c 64`. Do not configure two build dirs in parallel (the PSL step rewrites the shared tracked file), and restore that file afterwards if you do not want the diff.
Lesson: the old statement "TSan clean on the engine" covered tests with one I/O thread. Anything that was single-threaded by assumption ("loop thread only" comments) needs a check now that there are several loops: grep for function-level `static` state in `src/core`.

## Next
1. Ask the owner for `make bench` again (governor on `performance` if he can; default ONLY). Read: no error rows; 3.14t `def` pool rows (decide whether the free-threaded default pool should start at 2: +17 percent at 1 I/O thread, but a slower first burst for blocking handlers, since a step adds one thread per 500 us; a `grow_after` of 2 ms would add 4 per step, untested); `def JSON` plateau on 3.14t (suspects, untested: refcount contention on shared objects, pool queue lock; needs `perf`); inline non-monotone on 3.14t.
2. Review function-level statics and shared mutable state in `src/core` and `src/bindings` for the multi-loop case (the Date cache was one; there may be more that only show under free-threading or 8 loops). TSan the Python extension under load.
3. Competitors in C++ (the owner asked): Drogon, uWebSockets, libhv for speed; Crow, oat++, RESTinio, cpp-httplib, Boost.Beast for API; Python side: uvicorn (done), granian, Robyn. Offered, not started: add Drogon and uWebSockets to `benchmarks/` (needs system packages for Drogon, bench only). Wait for his answer.
4. WebSocket (RFC 6455) in the C++ core first, then the Python binding. ADR and tests first: handshake, frames (text, binary, continuation), masking, ping/pong, close codes, size limits, fragmentation, per-message limits, backpressure. It must run on the I/O thread like `route_io`, and the Python side follows the lanes design.
5. Small items: README gaps (HTTPException, response classes, status, 405), `listen()` via `signal.set_wakeup_fd`, thread-safety review of module level Python state under free-threading, FastAPI benchmark with `response_model` and dependencies.
6. CI: check that it is green (last failures: `test_the_command_line_takes_io_threads` fixed with PYTHONPATH; the macOS spread assertion is Linux only now).
7. Optional: callgrind of the C++ I/O path; a memory and sustained load test (the owner's reviewer asked for it).

## Not verified
Windows and macOS runs (macOS wheel tests had one failure, fixed, not re-run). Many-core scaling beyond the 4 physical cores the server gets in the bench. Behaviour on battery or with the performance governor. The Python test suite after the Date fix. Whether the Date race caused the two lost servers.
