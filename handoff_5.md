# httpp: hand-off #5 (for the next chat)

You are continuing work on **httpp**: a C++ HTTP library (server, client, downloads, progress bars, URL) with a Cython/Python layer on
top. Owner: the user (GitHub `mohammadraziei`). **Talk to the user in Persian**, keep reports short and concrete, and be honest about
numbers and limits. Everything inside the repo (docs, ADRs, comments, tests, changelog) is **English** and must contain **no em dash
character** (use a colon, comma or hyphen).

This document is the newest. Hand-off #4 (`handoff__1_.md`, and the older `handoff.md` inside the zip) stays true where it does not
conflict; its "what to do on the 2-core machine" section is now DONE (see below). The zip is not a git repository. Latest zip: `httpp_27.zip`.

Read first: `docs/agent.md`, `docs/Architecture.md` (risks 1 and 8), `checklist.md`, `docs/adr/README.md`, and the new
**ADR-0021** (`docs/adr/0021-pool-thread-state-and-two-core-numbers.md`), which holds every table quoted here.
Golden rule from `agent.md`: never take a **critical architecture decision** without asking the owner first, in the "options,
trade-offs, my recommendation" format of `agent.md` section 1. Non-critical steps: just do them and report. TDD: tests before code.

## Owner preferences

- Send the project back as a zip automatically after each step (no asking), excluding `__pycache__`, `build/`, `.git/`.
- "ponytail" style: simplest working solution, short explanations, a `ponytail:` comment on every deliberate simplification with its
  upgrade path. Full input validation, error handling and security still apply.
- Report at each step. Do not hedge about things tests pass. Record any xfail or skip in the ADRs.
- STL-like snake_case in C++. Zero system dependencies is the most important property.
- The owner reads Persian; he asked "I don't understand like this" when given terse tables: **compare against uvicorn and FastAPI and
  explain the cause in plain words**. He said "do it yourself" about the sync slowness and "continue improving", so non-critical
  performance work is pre-approved; critical decisions (below) are not.

## What was done in this session (all in the zip, all tested)

Machine: **2 cores**, CPython 3.13.16 (GIL) and 3.14.6t (free-threaded), `oha` built with `cargo install oha` (GitHub release download is
blocked in the sandbox), load generator in a separate process but on the same two cores, 32 connections, one worker, median of 3 rounds.

1. **Two-core measurement** (the point of hand-off #4). Findings: a CPU-bound `def` handler scales about 2.3x from 1 to 8 connections on
   3.14t and not at all on 3.13; `async def` handlers share one loop thread and gain nothing from free-threading (a small gain on 3.14t is
   probably I/O on the Asio thread, not investigated). Scripts: `benchmarks/cpu_app.py`, `benchmarks/cpu_bench.py`.
2. **Bug found by the measurement: `def` handlers were 5x slower on 3.14t** (1.9k req/s on `hello`, against 9.4k on 3.13). Cause: every
   pool-thread call used `with gil`, which creates and destroys a Python thread state per request. Fix: each pool thread keeps its
   state (`httpp_py_pin_thread` in `httpp_cy.pyx`, called at the start of the five dispatchers). Test: `tests/python/test_pool_thread_state.py`
   (uses `threading.local`, which lives on the thread state). Side effect on free-threaded CPython: an object released by another thread is
   freed only when its creating thread next runs Python. So a sync streaming generator is now closed explicitly on release
   (`provider.close = gen.close` in `_stream`, called by `_stream_release`) and the weakref assertion in
   `test_a_streamed_generator_is_released...` is skipped on free-threaded builds (the `finally` is checked instead). Recorded in ADR-0021.
3. **Worker pool rewrite** (`src/core/internal/worker_pool.hpp`). Before: one condition variable, threads woken in rotation, so a light load
   spread over all 8 threads that fought for the Python lock (a fixed pool of 1, 2, 4, 8 threads gave 34.9k, 29.8k, 22.5k, 19.9k req/s on
   `hello`, 3.13). Now: sleeping threads wake last-in first-out; at most as many threads are woken as there are queued jobs; a new job does
   **not** wake a second thread while another thread is running or about to run; a **scout** thread (asleep unless a job waits behind a busy
   thread) wakes or adds a thread when a job has waited 500 us (`help_after`). A blocked handler therefore delays queued work by about
   500 us. New `tests/cpp/test_worker_pool.cpp` (7 tests; 12 of 12 repeat runs pass; clean under ThreadSanitizer, compiled standalone
   with `g++ -I tests -I src/core ... tests/utest/utest_main.cxx`). Gotchas learned: a fresh pool's threads all grab jobs before they have
   gone to sleep (tests sleep 50 ms after construction); `wake_locked` must not add a thread after `shutdown` swapped `threads_`
   (it would terminate on a joinable thread).
4. **`async def` speedup** (Python layer only): one shared deadline timer (`_Deadlines` in `server.py`, `app._deadlines`) instead of a
   `call_later` per request; a prebuilt C JSON encoder (`_dumps`, same output as `json.dumps`, CPython only, a circular structure now
   gives RecursionError instead of ValueError, both end as a 500). Test: `test_finished_async_requests_leave_no_deadline_behind`.
5. **FastAPI added to the benchmark** (`benchmarks/fastapi_app.py`, `bench.py --targets fastapi,...`).
6. **Docs:** ADR-0021 (new, with all tables and the not-verified list), ADR index, `CHANGELOG.md` (Changed, Changed (async), Fixed),
   README "Speed" paragraph, `Architecture.md` risk 1 and risk 8 status.

Test state at the end: Python 3.13: 578 passed, 4 skipped; 3.14t: 579 passed, 3 skipped. C++: only `test_worker_pool` was run (standalone,
plus TSan). **The full C++ suite was NOT re-run** (about 4 minutes cold on 2 cores: do it first next time: see "Build and tool notes").
The last `server.py` change was tested by copying the file into the venvs' site-packages, not by rebuilding the wheel (C++ did not change
after the third rebuild). Not verified: Windows, macOS, abi3 wheel build in CI, more than 2 cores, a real network (client and server share
the machine), FastAPI with `response_model` or dependencies.

## Numbers (2 cores, 32 connections; compare ratios inside one run only, the box moves about 10 percent and at one point doubled for everyone)

Session start, `hello` req/s: FastAPI 4.6k, uvicorn 19.3k, httpp `def` 9.4k (3.13) / 1.9k (3.14t), `async def` 21k-28k.

After the thread-state fix, pool rewrite and delayed help (earlier machine state), `hello` / `item`:

| | uvicorn | httpp `def` | httpp `async def` |
|---|---|---|---|
| 3.13 | 17.2k / 11.9k | 35.9k / 24.9k | 25.2k / 19.8k |
| 3.14t | 18.2k / 14.0k | 33.2k / 29.8k | 22.8k / 21.8k |

After the async change (later, faster machine state, uvicorn itself doubled), `hello` / `item`:

| | uvicorn | httpp `def` | httpp `async def` |
|---|---|---|---|
| 3.13 | 34.0k / 22.8k | 61.9k / 55.3k | 47.5k / 43.7k |
| 3.14t | 31.9k / 26.1k | 62.1k / 57.2k | 48.7k / 44.8k |

Ratio of `async def` to uvicorn on `item` went from about 1.4 to 1.9, on `hello` from 1.1 to 1.4. FastAPI + uvicorn (same routes): 3.2k to 4.6k on
`hello`/`item`, 1.2k to 1.5k on the last of 300 routes (httpp is 5 to 17 times faster). CPU-bound handler (about 3 ms of Python), 1 / 8
connections: uvicorn 284 / 337 (3.13), 327 / 387 (3.14t); httpp `def` 3.14t 350 / 730-750; `async def` 349 / 398.

## Why free-threading did not speed up light handlers (the owner asked; answer already given in Persian)

The Python part of a light request is about 10 us (`json.dumps` a quarter of it), and the delayed-help pool now runs it on one hot thread
on both builds, so the Python work is effectively serial whether or not there is a GIL. The rest of the cost is the C++ Asio thread and
`oha`, which need no GIL and fight for the same two cores. Free-threaded CPython also has a small single-thread cost (atomic refcounts), and
3.13 vs 3.14 is not a controlled comparison. Free-threading pays only when the Python work per request is heavy (CPU-bound handler: 2.3x).

## The open problem: `async def` is still slower than `def` (47.5k against 61.9k on `hello`, 3.13)

Do not start on this without the owner: it is **Architecture.md risk 1** and a critical decision (concurrency model, layer boundary).
Correction of something I said wrongly mid-session: options B and C of risk 1 (dispatch on the I/O thread, indexed router, build only the
fields needed) were **already built** (ADR-0009); what remains is different.

How the async path works: the Asio I/O thread parses the request and pushes it into the inbox (`route_io`), writes a byte to a socket pair
if needed; the asyncio loop thread wakes in `epoll`, `drain()` pops requests and `dispatch()` creates one Task per request; the handler's
answer goes through `done(status, body, headers)` which crosses into C++ (a `with gil` block on the loop thread) and posts the write to the
Asio thread (another wake-up). So per request there are two thread wake-ups (batched under load), one Task, and one crossing into C++.
There is only one Python thread, so this is NOT the Python-lock handover that hurt `def`.

Findings so far (py-spy `--gil`, loop thread, Python side only): `call_later` 9 percent, `create_task` 9 percent, `json.dumps` 14 percent,
`_to_response` 17 percent, `drain`/`on_wake` 15 percent. The first and third are fixed. **Hypotheses, not proven:**

- H1: the wake-up byte plus `epoll` per burst and the `eventfd`/post wake-up of the Asio thread per response cost most of the remaining gap
  (two wake-ups per request when the system is not saturated). Test: count syscalls with `strace -c -f` on both servers under the same load, or
  `perf` if available (it was not installed; `gdb` and `strace` were).
- H2: Task creation (about 9 percent) could be avoided for coroutines that finish without suspending (run the first step by hand with
  `coro.send(None)`, create a Task only if it suspends). `eager_task_factory` was tested earlier and made no difference (Architecture.md risk 1),
  but that was with the per-request timer present; re-test.
- H3: part of the gap is simply that `def` now runs on a pool thread with no asyncio machinery at all (no loop iteration, no Task, no
  callbacks), so a trivial async handler can never beat it; the fair comparison is a handler that does I/O, which was NOT measured (hello and
  item have no I/O, which is exactly where `async def` has no advantage). A benchmark with `await asyncio.sleep(0.01)` or a real socket and
  hundreds of concurrent connections would show where `async def` wins (threads cost memory and the pool is capped at about 4x base).

Options to put in front of the owner (use the `agent.md` format; my recommendation last):

| Option | Gain | Cost / risk |
|---|---|---|
| A. Keep as is | none | `async def` slower than `def` on trivial handlers; confusing but harmless |
| B2. Avoid the Task for non-suspending coroutines (H2) | maybe 5 to 10 percent | Python layer only, but cancel/timeout semantics must stay exact |
| C. Batch responses: collect `done` results during one loop iteration and hand them to C++ in one call (one lock, one wake-up) | fewer wake-ups (H1) | changes the `done` protocol between the layers (internal), small added latency for the first response of a burst |
| D. Run the asyncio loop on the Asio I/O thread (uvicorn's model, no hops) | largest | breaks on IOCP (Windows) and is hard on kqueue; Python handler time would block all socket I/O; **recommend rejecting** |
| E. Several loops (`Server(loops=N)`, opt-in, default 1) | `async def` scales on 3.14t | breaks the documented promise that all requests share one loop (a shared `aiohttp.ClientSession` or async DB pool); critical, API-level |
| F. uvloop | maybe | a dependency, violates zero dependencies; reject |

Recommendation: first measure H1 with `strace -c`; if wake-ups dominate, do C; do B2 as a non-critical Python-only step after re-testing H2;
leave E as an owner decision for when the owner cares about multi-core `async def` on free-threaded builds; reject D and F.

## Other pending work (the older list, updated)

1. Run the **full C++ suite** and the abi3 wheel check; rebuild the wheel from source once to confirm what was only copied.
2. Thread-safety review of module-level Python state under the free-threaded build (`_tasks`, `_loop`, handler tables, and now
   `app._deadlines`, which is loop-thread only by design) plus a longer stress run (the 16-thread stress script from hand-off #4 was not rerun;
   rerun it on 3.14t with the new pool).
3. A benchmark with I/O in the handler (see H3), and FastAPI with `response_model` and dependencies, to give the owner a fair picture.
4. `def` `item` and `hello` pool tuning knob: `help_after` is a constant (500 us). Consider exposing it only if someone needs it.
5. The first batch of 8 concurrent blocking handlers after server start takes 0.49 s against 0.21 s later (cause not looked into: probably
   first-use warm-up, but check it was not made worse by the scout).
6. From hand-off #4: `response: Response` injected parameter, `background=`, a full `Request`; a total deadline for a slowly dripped request
   body; `HTTPException` for `APIRouter`, `BackgroundTasks`, middleware, forms and uploads, GZip middleware; README sections for
   `HTTPException`, response classes, `httpp.status` and the 405 behavior; WebSocket (owner said "not now"); Swagger UI body copied per
   request; real zlib instead of the stb codec (owner said drop it for now).
7. `--workers N` as threads instead of processes on free-threaded builds (`app.shared` idea): still an owner decision (hand-off #4, item 3).

## Build and tool notes

- Venvs used (may be gone in a new sandbox): `uv python install 3.14t`; `uv venv --python 3.14t venv14t`; `uv venv --python 3.13 venv13`;
  `uv pip install cmake ninja cython jsonschema jinja2 cython-cmake pytest scikit-build-core pydantic uvicorn uvloop httptools fastapi`, then
  `uv pip install /path/to/httpp` (about 4 to 6 minutes on 2 cores; run both builds in the background with `setsid nohup` and poll).
  `sys._is_gil_enabled()` must print False on 3.14t after `import httpp`.
- A pure-Python change can be tested by copying `src/bindings/python/httpp/server.py` over the installed copy in `site-packages/httpp/`.
- Benchmarks: `python benchmarks/bench.py --oha <oha> --targets uvicorn,fastapi,httpp,httpp-async --only hello,item,many`;
  `python benchmarks/cpu_bench.py <oha>`. `oha`: `cargo install oha --root <dir>`.
- Profiling: `pip install py-spy`; `py-spy record --gil -d 8 -r 250 -f raw -o out.txt --pid <pid>` while `oha` runs from another process
  (without `--gil` almost every sample is an idle thread; py-spy also slows the server a lot, so use it for shares, not for speeds).
- Standalone pool tests: `g++ -std=c++17 -O1 -pthread -I tests -I src/core tests/cpp/test_worker_pool.cpp tests/utest/utest_main.cxx -o wp`
  (add `-g -fsanitize=thread` for TSan).
- C++ full build and Python build commands, gotchas (`src/core/internal` includes, `tests/cpp/CMakeLists.txt` globs `test_*.cpp`, utest
  `%` in `ASSERT_*`, no typed memoryviews in `.pyx`, abi3 wheel only in CI) are as in hand-off #4.
- Sandbox limits seen: the proxy allows package registries (PyPI, crates.io) but not GitHub release downloads; the tool-call classifier
  timed out twice (just retry).
