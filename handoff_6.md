# httpp: hand-off #6 (for the next chat)

You are continuing work on **httpp**: a C++ HTTP library (server, client, downloads, progress bars, URL) with a Cython/Python layer on top.
Owner: the user (GitHub `mohammadraziei`). **Talk to the user in Persian**, keep reports short and concrete, and be honest about numbers and
limits (he catches overclaiming: he pushed back twice this session and was right both times). Everything inside the repo (docs, ADRs,
comments, tests, changelog) is **English** and must contain **no em dash character** (use a colon, comma or hyphen).

This document is the newest. Hand-off #5 (`handoff_5.md`) and older ones stay true where they do not conflict. The zip is not a git
repository and nothing was ever pushed by the assistant: **the owner commits and pushes himself**; never push.

Read first: `docs/agent.md`, `docs/Architecture.md` (risks 1 and 8), `checklist.md`, `docs/adr/README.md`, and **ADR-0021**
(`docs/adr/0021-pool-thread-state-and-two-core-numbers.md`), which holds every table and the reasoning of the last two sessions.
Golden rule from `agent.md`: never take a **critical architecture decision** without asking the owner, in the "options, trade-offs, my
recommendation" format of `agent.md` section 1. This session the owner explicitly delegated one such decision ("decide the right thing");
that is recorded in ADR-0021 and does not carry over to other decisions. Non-critical steps: just do them and report. TDD: tests before code.

## Owner preferences

- After every step send the whole project back as a zip, automatically, excluding `__pycache__`, `build/`, `.git/` (command used:
  `cd <parent> && zip -qr httpp.zip httpp -x '*/__pycache__/*' '*/build/*' '*/.git/*'`, then `SendUserFile`).
- "ponytail" style: simplest working solution, short explanations, a `ponytail:` comment on every deliberate simplification with its upgrade
  path. Full input validation, error handling and security still apply.
- Report at each step; do not hedge about things tests pass; record any xfail or skip in the ADRs.
- STL-like snake_case in C++. Zero system dependencies is the most important property.
- He reads Persian and asks "I don't understand" when given terse tables: explain causes in plain words, compare against uvicorn and FastAPI.
- He answers "continue" or "you decide" when he does not want to choose. Then decide, say what you decided and why, and record it.

## What was done in this session (all in the zip)

State at the end: C++ suite 263 of 263 (run after the last C++ edit, the brace fix), Python 3.13: 582 passed, 4 skipped; 3.14t:
583 passed, 3 skipped; `benchmarks/stress.py 20 16`: 3.13 about 51k answers, 3.14t 74k to 77k, no wrong answer, no unexpected stderr.

1. **`async def` speed** (ADR-0021 has the numbers). Hand-off #5 left it open as "async is slower than def". Findings: syscalls per request are
   the same as uvicorn (3.1 against 3.2), so extra wake-ups were not the cost. Per request CPU: `async def` 29.5 us, `def` 20.1 us; about
   7 us of the gap is Python (asyncio machinery), 2.3 us the C++ side. Native timers showed about 5.7 us of about 18 us on the loop thread
   is the Python/C++ boundary (`inbox_pop` 2.5 us, `send` into C++ 2.9 us). Changes: eager task start on 3.12+ (`dispatch` in `server.py`),
   the inbox is drained in batches under one lock (`httpp_py_inbox_pop_many` in `httpp_cy.pyx`, same arming rules), `_to_response` fast path
   for exact `str`/`dict`/`list`, the `handler_timeout` deadline is registered only for a task that did not finish at once. Together about
   20 percent on `hello` / `item` (3 alternating A/B rounds on a noisy 2-core box; compare ratios only inside one pair).
2. **I/O-bound benchmark** (`benchmarks/io_app.py`, `io_bench.py`): with a 10 ms wait, `def` is capped at about 3.1k req/s by the pool
   (about 32 threads) while `async def` reaches about 32k at 512 connections (uvicorn 26k). Rule of thumb: `def` for CPU work or blocking
   libraries, `async def` for waiting on the network. **Not yet in the README** (see pending item 1).
3. **Option C (batching answers into one C++ call) was NOT taken**: it could save about 2 us of about 18 us, changes the internal `done`
   protocol and adds latency to the first answer of a burst. Reopen only if `async def` throughput becomes the limit.
4. **macOS CI build fix:** wheels target macOS 10.13 and `std::filesystem` needs 10.15. `inside_dir` in `src/core/server.cpp` now uses
   `realpath` on Linux and macOS (`std::filesystem` stays on Windows only). Tests cover a sibling directory with the same prefix and a
   mounted directory given with a trailing slash. `-Wdangling-else` warning in `async_server.cpp` fixed with braces (the `HTTPP_LOG` macro
   ends in an `else`; never use it as the unbraced body of an `if`).
5. **A real deadlock in `Server.listen()` on free-threaded Python** (found from a CI thread dump on macOS cp315t, test
   `test_interrupt_before_the_port_is_bound_still_stops_listen`): a KeyboardInterrupt landing inside `threading.Event.wait()` can leave the
   Event's lock taken or released twice, so `listen()` and the serving thread block on it for ever. Reproduced stand-alone on Linux 3.14t
   (`RuntimeError: release unlocked lock`), never on 3.13. Fix: `listen()` polls a plain list flag every 50 ms with `time.sleep`.
   Tests: `test_listen_waits_without_an_event_or_a_condition` (deterministic, red before the fix) and `test_ctrl_c_at_any_moment_never_wedges_listen`
   (stress; it did NOT fail on the old code on Linux because the race window is tiny, so the deterministic one is the real guard).
   My first guess (worker pool shutdown race) was wrong; the thread dump settled it.
6. **CI changes** (`pyproject.toml`): `pytest-timeout` in the `test` extra; test command
   `pytest {project}/tests -o faulthandler_timeout=120 --timeout=600 --timeout-method=thread --durations=20`. The hard timeout is a safety net,
   not a cure: it turns a hang into a failure with all thread stacks. `test_embed_file` compile tests are skipped when `CIBUILDWHEEL` is set
   (they compile a C++ program; the native cmake workflow still runs them). I claimed they were slow under emulation; **that was not proven**
   (cp39 macOS tests finish in 54 and 81 seconds, so macOS is not generally slow). Treat the skip as a pragmatic choice, not a diagnosis.
7. `benchmarks/stress.py`: stderr of the server now goes to a file (with a pipe nobody reads, the server blocks on its own log after about
   64 KB and the run "hangs" after about 1100 requests); `STRESS_KINDS=s,a,...` limits the request kinds. The server itself does not guard
   against a full stderr pipe (same as any server that logs to stderr).

## Pending work (ordered)

1. **Confirm CI is green on all three OS with the `listen()` fix, several times.** The hang was a race, so one green run proves little:
   the owner saw a green run before the last failing one. Last known CI: Ubuntu green (cp39, cp314t, cp315t); macOS cp39 and cp314t green,
   cp315t failed only on the hang that is now fixed; Windows was not looked at this session. If a test hangs again, ask for the `Timeout`
   thread dump (it names the blocked frames).
2. **README paragraph**: when to write `def` and when `async def` (item 2 above). Non-critical; I told the owner I would do it unless he
   objected, and he did not answer. Also README/docs mention of the 50 ms stop latency of `listen()` is not needed.
3. **FastAPI benchmark with `response_model` and dependencies** for a fair picture (`benchmarks/fastapi_app.py` has only the plain routes).
4. The remaining pure-Python cost of an `async def` request is about 11 us (`prepare`, `run`, deadlines, `_to_response` for other types).
   Non-critical Python-only work; profile with `py-spy record --gil` first.
5. The ThreadSanitizer build of the C++ tests was not re-run after the `worker_pool` rewrite of hand-off #5 beyond the standalone pool test; run
   the full suite under TSan once (about 4 min cold on 2 cores for the plain build, more with TSan).
6. Thread-safety review of module-level Python state under free-threading (`_tasks`, `_loop`, handler tables, `app._deadlines` is loop-thread
   only by design). The stress run passed on 3.14t three times; a longer run and a 3.15t run (not available in the sandbox) are still open.
7. The first batch of 8 concurrent blocking handlers after server start takes 0.49 s against 0.21 s later (cause not looked into).
8. From hand-offs #4 and #5, still open: `response: Response` injected parameter, `background=`, a full `Request`; a total deadline for a slowly
   dripped request body; `HTTPException` for `APIRouter`, `BackgroundTasks`, middleware, forms and uploads, GZip middleware; README sections
   for `HTTPException`, response classes, `httpp.status` and the 405 behavior; Swagger UI body copied per request; real zlib instead of the stb
   codec (owner said drop it for now); WebSocket (owner said "not now"); `--workers N` as threads on free-threaded builds (owner decision).
9. Possible follow-up: `listen()` could use `signal.set_wakeup_fd` instead of polling (upgrade path in the `ponytail` comment).

## Not verified

Windows (runtime), macOS runtime beyond what CI runs, more than 2 cores, a real network (client and server share the machine), Python 3.15t
locally (only CI has it), the removal of the `-Wdangling-else` warning on the CI compiler (local gcc 13 never emitted it, braces remove the
ambiguity by construction), FastAPI with `response_model` or dependencies.

## Build and tool notes

- Sandbox: 2 cores, CPython 3.13.16 and 3.14.6t via `uv`; venvs may be gone: `uv python install 3.14t`; `uv venv --python 3.14t venv14t`;
  `uv venv --python 3.13 venv13`; `uv pip install cmake ninja cython jsonschema jinja2 cython-cmake pytest pytest-timeout scikit-build-core
  pydantic uvicorn uvloop httptools fastapi`; then `uv pip install --no-build-isolation /path/to/httpp` (1.5 to 2.5 minutes each; run in the
  background with `setsid nohup` and poll). `sys._is_gil_enabled()` must print False on 3.14t after `import httpp`.
- A pure-Python change can be tested by copying `src/bindings/python/httpp/server.py` over the installed copy in `site-packages/httpp/`;
  a `.pyx` change needs the wheel rebuilt (`--reinstall`).
- C++ tests: `cmake -S . -B ../build_cpp -G Ninja -DCMAKE_BUILD_TYPE=Release -DHTTPP_BUILD_PYTHON=OFF -DHTTPP_BUILD_TESTS=ON`, then
  `cmake --build ../build_cpp -j2` and run `tests/cpp/test_httpp_cpp`. Standalone pool tests: see hand-off #5.
- `oha` was built with `cargo install oha --root <dir>` (GitHub release downloads are blocked). Benchmarks: `benchmarks/bench.py`,
  `cpu_bench.py`, `io_bench.py`; stress: `benchmarks/stress.py [seconds] [threads]`.
- Profiling that worked: `py-spy record --gil [--native] -r 300 -f raw -o out --pid <pid>` while `oha` runs; `strace -c -f -p <pid>`;
  per-thread CPU from `/proc/<pid>/task/*/stat`; temporary nanosecond timers inside a copy of `httpp_cy.pyx` (build the copy in its own
  venv, never in the deliverable). `perf` is not installed. Do not use tool calls that need `perf`.
- macOS wheels target 10.13 (`MACOSX_DEPLOYMENT_TARGET` in `.github/workflows/wheels.yml`): avoid `std::filesystem`, floating-point
  `to_chars`, `std::optional` accessors and other post-10.13 libc++ APIs in `src/core`.
- The tool-call classifier timed out several times (just retry); outbound traffic goes through an allowlist proxy (PyPI and crates.io work).
