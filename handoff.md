# httpp — hand-off #2 (for the next chat)

You are continuing work on **httpp**: a C++ HTTP library (server, client, downloads, progress bars, URL)
with a Cython/Python layer on top. Owner: the user (GitHub `mohammadraziei`). **Talk to the user in
Persian**, keep reports short and concrete, and be honest about numbers and limits (see section 8).

The owner says everything described here is committed and pushed. This document supersedes the first
hand-off; everything in it that is still true is repeated here.

**Also read `docs/agent.md`, `docs/Architecture.md`, `docs/checklist.md`.** The owner drafted them with
DeepSeek and asked for them to be reviewed; they were corrected against the real code (wrong claims about
cpp-httplib as the server base, proxy being out of scope, "one wheel everywhere", GoogleTest instead of
utest, invented compatibility numbers, and so on were fixed). Their **golden rule overrides "do not ask
permission before every step" for critical decisions**: never take a critical architecture decision (the
open risks in `Architecture.md` section 7: async hop, shared-memory library and safety, coupling, FastAPI
level, WebSocket backpressure, cross-platform strategy) without asking the owner first, in the format shown
in `agent.md` section 1 (options, trade-offs, your recommendation). Non-critical steps: just do them and
report. TDD applies: tests before code.

**Current state of the conversation:** the owner asked to *stop* and write this hand-off. They have **not**
yet confirmed the priority order in section 6. Do not start section 6 work until they say which item.

## 0. Get the project (do this first)

```sh
git clone --recurse-submodules --shallow-submodules -j8 https://github.com/mohammadraziei/httpp.git
cd httpp
```

Do not ask for a zip; the repository is the source of truth. Quick sanity check that this session's work
is in the clone, then confirm the baseline (**179 C++ tests, 155 Python tests pass + 1 skipped** on a
normal Python; the skipped one is the free-threading guard):

```sh
ls src/bindings/python/httpp/_run.py tests/python/test_run.py
grep -n "set_reuse_port" include/httpp/server.hpp
grep -n "freethreading_compatible" src/bindings/python/httpp_cy.pyx
```

If something is missing, tell the user which files are absent and ask them to push; do not silently redo it.

## 1. Hard rules from the owner

- **Zero system dependencies**: nothing installed with `apt`/`brew`/a system package. pip *build*
  dependencies are fine (`jsonschema`, `jinja2`, `cython>=3.1` are in `pyproject.toml`
  `[build-system] requires`). Every new dependency must be vendored or be a git submodule.
- **Naming: STL-like `snake_case`** for types, functions, variables, enumerators.
- **Coding style "ponytail"** (github.com/DietrichGebert/ponytail): simplest working solution, reuse
  stdlib/existing code, no unrequested boilerplate, fix root causes, short explanations (code first),
  mark deliberate simplifications with `// ponytail:` / `# ponytail:` naming the upgrade path. Still do full
  input validation, error handling and security. (The owner also said: when correctness needs more work,
  do the correct thing; do not take shortcuts that are really tricks.)
- **cpp-httplib stays pristine**: `src/third_party/cpp-httplib/httplib.h` is byte-identical to upstream
  v0.54.1 (re-fetch with `wget` from
  `https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.54.1/httplib.h`). Never patch it. Only the
  synchronous client, `curl.cpp` and `download.cpp` use it; it is to be retired later.
- Vendored code: `src/llhttp` (unmodified v9.4.3), `src/third_party/aixlog` (one `[httpp patch]`),
  Asio, mbedtls and liburlparser are submodules. Document any vendor change in `src/third_party/README.md`.
- **Reports and delivery:** after each step give a short report (what was done, tests, what is not done,
  honest limitations). **Report the design choices you make** (the owner asked for this explicitly), with
  the reason. When asked for output, **zip the whole project** (not a diff) without `build*/`, `.git`,
  `__pycache__`, and present it, without asking first. Update the README when behaviour changes.
- Be honest about "async" and about the GIL: say what is truly non-blocking and what is a thread pool.

## 2. The owner's direction (decided this session)

- **httpp replaces both layers: FastAPI/Starlette above, uvicorn below.** The API is Starlette-like (FastAPI-like
  on top), implemented **natively** on our engine; because we own both layers, shared memory, declarative work in
  C++ and everything else aimed at escaping the GIL is designed across the whole stack. Compatibility is measured by
  running Starlette's, then FastAPI's, own test suites against httpp (import shim + an `httpp.testclient.TestClient`
  over a real loopback server; see `docs/agent.md` section 6). **No ASGI adapter is needed for that**; I wrongly said
  otherwise earlier and the owner corrected me. An ASGI adapter is only for running third-party ASGI apps: low priority.
  How far the API goes (Starlette-only, FastAPI-like too, pydantic as optional dependency) is still open (Risk 4).
- Launched by `httpp run main:app` or `app.run()` in `if __name__ == "__main__"`.
- Wants sync and async handlers handled efficiently, with as little GIL contention as possible, by the most
  correct hybrid C++/Cython design, with a simple obvious API.
- Rejected after discussion: an asyncio-compatible event loop written on Asio (uvloop-style: too heavy) and
  a C++ coroutine runner that only understands our own awaitables (incompatible with httpx/asyncpg/anyio).
  The handler loop is a **standard asyncio loop**.

## 3. Architecture (what changed since hand-off #1)

```
include/httpp/        public API (server.hpp now has set_reuse_port)
src/core/             async_server.cpp (Asio loop thread + llhttp; set_reuse_port), server.cpp (router,
                      hooks, static files; handlers run on a worker pool), async_http.cpp (async client),
                      client.cpp/download.cpp/curl.cpp (httplib, sync), log.cpp, tls_session.cpp
src/bindings/python/
  httpp_cy.pyx        Cython. NEW: "loop inbox" (C++ mutex+deque, wake byte to a socket), add_loop_route,
                      _attach_inbox/_drain_inbox, _start_dispatch (shared by pool and inbox paths),
                      Server.set_reuse_port. Header: `# cython: language_level=3, freethreading_compatible=True`
  httpp/server.py     Server, route adapters (_adapt_async/_adapt_sync), _get_loop() (selector loop +
                      socketpair + add_reader), Server.run
  httpp/_run.py       NEW: run(), load("mod:attr"), _supervise() for --workers
  httpp/__main__.py   CLI: server, download, curl, and NEW `run`
tests/cpp/            +1 test (reuse_port); tests/python/test_run.py NEW; test_server_features.py +4 tests
```

Request paths today (this is the key to the performance picture in section 5):

- **Sync `def` route:** Asio loop thread → C++ worker pool thread → takes the GIL → runs `_prepare` + handler
  → `done`. One thread hop.
- **`async def` route:** Asio loop thread → worker pool thread (routing, before-hooks run there) →
  `inbox_push` (no GIL; writes one wake byte only when the queue goes empty→non-empty) → the single asyncio
  loop thread wakes, takes the GIL once per burst, `_drain_inbox` (budget 256 per round, then `call_soon`
  re-arm) → `create_task` → handler → `done(...)` called directly on the loop thread. Two thread hops.
- If the app has any `after_request` or `error_handler`, `done` goes through `run_in_executor` instead,
  because those hooks run *inside* `done()` on the calling thread and an `async def` hook would wait on the
  very loop that called it (deadlock; proven by forcing the direct path). `# ponytail:` marked; upgrade path:
  await the hooks inside the coroutine.
- Python request objects: `_request_args(req)` builds 6 Python objects per request and `_prepare` builds a
  `ServerRequest` for **every** request even when the handler does not take `request`.

## 4. Build and test

```sh
pip install -r requirements-dev.txt
pip install cmake ninja --break-system-packages     # sandbox: they land in /usr/local/bin
export PATH=$PATH:/usr/local/bin:$HOME/.local/bin
cmake -B build -G Ninja -DHTTPP_BUILD_TESTS=ON -DHTTPP_BUILD_PYTHON=OFF && ninja -C build
build/tests/cpp/test_httpp_cpp                      # 179 tests

cmake -B build-py -G Ninja -DHTTPP_BUILD_PYTHON=ON -DHTTPP_BUILD_TESTS=OFF && ninja -C build-py
PYTHONPATH=build-py/python/install python3 -m pytest tests/python -q     # 155 passed, 1 skipped
```

Real wheels (what CI builds), with the sandbox's `uv`:

```sh
pip install uv --break-system-packages
uv build --wheel --python 3.12  --out-dir /tmp/abi3wheel --config-setting=build-dir=/tmp/abi3build .   # cp39-abi3
uv python install 3.14t                                                                                  # free-threaded interpreter
uv build --wheel --python 3.14t --out-dir /tmp/ftwheel   --config-setting=build-dir=/tmp/ftbuild .       # cp314-cp314t
uv venv --python 3.14t /tmp/ftenv && uv pip install --python /tmp/ftenv/bin/python /tmp/ftwheel/*.whl pytest
cd /tmp && /tmp/ftenv/bin/python -m pytest /path/to/httpp/tests/python -q      # run from outside the repo
```

Gotchas learned (new this session, plus the old ones that still apply):

- Long builds die when the tool call ends unless detached:
  `setsid nohup bash -c '... > /tmp/log 2>&1; echo FINISHED >> /tmp/log' &` then poll. A first full build
  is several minutes; incremental builds with a fixed `build-dir` are fast.
- After editing a `.py` file under `src/bindings/python`, run `ninja -C build-py` to copy it into
  `build-py/python/install` before running pytest. Use an absolute or repo-relative `PYTHONPATH` only for
  in-process tests; `tests/python/test_run.py` starts child `python -m httpp` processes in another cwd and
  derives their `PYTHONPATH` from `httpp.__file__` (`child_env`).
- **Never `pkill -f <pattern>` that appears in your own command line**; kill by PID.
- utest `ASSERT_*` may evaluate twice on failure: no side effects inside asserts; `std::string` is not
  printable by `ASSERT_EQ`, use `ASSERT_TRUE(a == b)`.
- `Response.text` is a **property** (not a method). `httpp.post(url, data=..., content_type=...)`, not `body=`.
  Python handlers take arguments by name; header keys in `request.headers` are lower-case.
- `Server()` creates an Asio epoll fd at construction: **never `fork()` after creating a `Server`**
  (children would share that epoll instance). That is why workers are fresh processes.
- One flaky run was seen once: `test_keyboard_interrupt_stops_listen` failed in the very first cold run and
  never again in ~10 later runs. Not understood; if it shows up again, investigate `listen()`'s interrupt path.
- Sandbox: **1 CPU core**, Python 3.12.3. Outbound network: GitHub (incl. release assets, so `uv python
  install` works), PyPI and npm work; most other hosts do not. A single core makes multi-core claims
  impossible to verify here.
- The benchmark client in `/tmp` reconnects every 90 requests because the server's keep-alive limit is 100.

## 5. Results of this session

**Tests:** C++ 178 → 179, Python 139 → 155 passed + 1 skipped (156 on a free-threaded interpreter).
`test_run.py` (13 tests) also passes on 3.14t, including the GIL guard.

**Performance (single core, trivial handler, same Python load generator, 8 connections, 4 s, 3 rounds):**

| path | req/s |
|---|---|
| `async def`, previous path (pool thread → `run_coroutine_threadsafe`) | ~10.9k |
| `async def`, loop inbox (this session) | ~17.5k |
| `def` (sync) | ~29k |
| uvicorn 0.54 (httptools + uvloop), hello world | ~24k |

So today: `def` is ~20% faster than uvicorn, **`async def` is ~28% slower** than uvicorn. Caveats: 1 core,
empty handler, Python client, no `wrk`/`oha`. This says nothing about multi-core or real apps.
Hypothesis **disproved**: `asyncio.eager_task_factory` made no measurable difference (reverted). The gap is
~24 µs per request and is most likely the extra thread hop plus wake syscalls and GIL handoff (uvicorn does
everything on one thread); this is a hypothesis, not yet proven.

**Free-threaded Python:** without the Cython directive, importing the extension on 3.14t **re-enables the GIL**
(RuntimeWarning). With `freethreading_compatible=True` the GIL stays off (`-W error` import clean); the whole
Python suite passed on 3.14.4t; a mixed sync/async/client stress test (16 threads, 2400 requests, run 3 times)
had zero errors. Not measured: any multi-core speed-up.

**Wheels:** `cp39-abi3` (all ordinary 3.9+) built and tested on 3.12; `cp314-cp314t` built and tested.
Free-threaded Python has **no stable ABI** on 3.13t/3.14t; `abi3t` (PEP 803) starts with 3.15
(`cp315-abi3.abi3t`) and needs Cython and scikit-build-core support (not checked beyond the PEP text).
scikit-build-core ignores `py-api` for `t` builds, so the existing `py-api = "cp39"` is unchanged.

## 6. Remaining work, in priority order (owner has NOT confirmed this order)

Scores are importance/effort out of 10.

1. **Remove one thread hop for `async def`** (7/5): route-match on the Asio thread and push straight to the
   inbox when the route is async and the app has no `before_request` hook; today `server.cpp::handle` runs on
   the pool first. Measure with the benchmark above (target: close the gap to uvicorn; do not promise numbers).
2. **Build only what the handler asks for** (6/3): `_prepare` always builds a `ServerRequest`, `_request_args`
   always builds 6 objects. Decide at call time (exception handlers may be registered after the route and
   need the raw request). Measure before/after.
3. **`--workers auto`** (6/1): `os.cpu_count()` on a normal interpreter; on a free-threaded one, one process
   (threads run in parallel). Detect with `sysconfig.get_config_var("Py_GIL_DISABLED")`.
4. **Shared state across workers, `app.shared`** (7/8). The owner considers it the most important item.
   Be honest in the report: it adds a *capability* (workers without Redis), not raw speed; it can beat
   uvicorn+Redis on stateful workloads, which is **not measured**. Suggested minimal design: the supervisor
   creates a shared mapping (`memfd_create`/`shm_open`, Linux only) before spawning workers; a bytes→bytes
   table with an inter-process lock plus atomic counters (`get/set/incr`); Python objects only through
   explicit serialization (JSON); test with 3 workers. Not started.
5. **Benchmark on a multi-core machine** (8/2): `wrk`/`oha`, three modes: `cp39-abi3` single process,
   the same on 3.14t, and `--workers N`; also compare with `uvicorn --workers N`. Without this no "faster than
   uvicorn" claim is valid.
6. `cpu=True` route option with an automatic process pool (4/8): later.
7. **Server connection cap** (`set_max_connections`; today only fd-exhaustion backoff), slow-request limits,
   per-route timeout (security, was priority 2 in hand-off #1).
8. `--loop uvloop` option for the handler loop (cheap, optional), `httpp run --reload`, respawn of dead
   workers (currently the group stops with exit code 1), `PR_SET_PDEATHSIG` so workers die with the supervisor.
9. Python sub-interpreters experiment (offered to the owner, not answered): one process, several GILs, with
   the C++ engine distributing requests. Needs the Cython module to be isolation-safe; large.
10. Older list, unchanged: `Download.run_async` on the engine (streaming to file, Range/resume, progress,
    timeout), retire httplib (after the previous item), WSGI/ASGI adapters (now low priority), server extras
    (TLS, WebSocket, compression, multipart, request body streaming, client-disconnect detection, access
    log, HTTP/2), real request cancellation, cookie jar for sessions, thread-free name resolution.
11. Verification not done: CI for **macOS and Windows** (system roots, `crypt32`, and the wake-socket code
    for the loop inbox are written but never run), ThreadSanitizer/ASan on server and engine, running the
    new `wheels.yml` matrix.
12. Small items: stale comments in `progress.hpp` (~lines 16, 43); 404-vs-405 for a wrong method; no symlink
    protection in static serving; no `Server:` header; example of TLS client use.

Considered and dropped: typed path params (`{id:int}`) validated in C++ (saves GIL only for *invalid*
requests; valid ones need Python ints anyway); `eager_task_factory` (no gain).

## 7. Behaviour worth remembering (new)

- `httpp.run(app_or_"mod:attr", host="127.0.0.1", port=8000, *, workers=1, reuse_port=False,
  log_level=None)`; `app.run(...)` and `httpp run main:app [--host --port --workers --reuse-port --log-level]`
  are the same function. Validates host/port/workers; port must be 1..65535. Stops on Ctrl+C **and SIGTERM**
  (SIGTERM → `signal.default_int_handler`), gracefully (`on_shutdown` hooks run). Bad target → short error,
  exit 1.
- `workers > 1`: needs an import string; the supervisor starts N fresh `python -m httpp run ... --reuse-port`
  processes in their own sessions, sends each one SIGTERM on stop, kills after 10 s, and if any worker dies it
  stops the group and exits 1 (no respawn). Linux only (clear `OSError` elsewhere). Killing the supervisor
  with SIGKILL leaves workers running. Module-level state is **per worker**.
- `reuse_port` is off by default (it also lets an unrelated same-user process bind the port silently). The
  C++ test `reuse_port_lets_servers_share_a_port_only_when_both_ask` is Linux-only.
- `Server(threads=N)` sets the worker pool size; plain `def` handlers take the GIL on those threads.
- Redirects off by default (max 20 hops when on); async client default timeout 300 s per step; a dead
  keep-alive connection is retried once only for idempotent methods; default log level is warning.

## 8. Honest limitations to keep stating

Handler code is Python and needs the GIL (one process runs one handler's Python at a time) unless the
interpreter is free-threaded or you use `--workers`. Free-threaded results come from one core only. The
benchmark numbers above are hello-world on one core. `async def` is currently slower than uvicorn.
`--workers` is Linux only and does not respawn. Windows/macOS parts are unverified. Handlers on a
free-threaded interpreter must be thread-safe (shared dicts/counters are touched by several threads).

## 9. Questions for the owner

1. Confirm or change the priority order in section 6 (they leaned towards item 4 first; the data says items
   1 and 2 give the speed, item 4 gives the capability).
2. Sub-interpreter experiment (item 9): yes or no?
3. Shared-state scope for item 4: bytes table + counters only, or also something richer?
