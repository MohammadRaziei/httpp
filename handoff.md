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

**Current state of the conversation (updated):** the owner has since decided **option B + C** for the hot
path (items 1 and 2 of section 6), and told me not to worry about implementation cost: choose the best, most
principled way, no laziness. Both are **done** (see "Update: hot path rebuilt" below). They have **not**
confirmed the order of the remaining items (3 onward in section 6). Items that are critical architecture
decisions (the open risks) still need the `agent.md` question format before any code.

## Update 2: FastAPI-compatible layer (STARTED, about a third done)

**Decision (owner):** the "native compatible subset": implement `Depends` and Pydantic-like models ourselves,
"as far as possible" (the owner also said: do whatever you like, best and most principled way, no laziness).
The point: FastAPI-style code runs on httpp's own server with no ASGI adapter, so it keeps the speed. ASGI
adapter was the other option and is NOT built. Compatibility will never be complete; keep a list of what is not.

### Done (files are in the zip)

- `src/bindings/python/httpp/_validate.py`: compiles a type hint into a validator closure once; Pydantic v2
  "lax" coercion and Pydantic-shaped errors (`type`, `loc`, `msg`, `input`, `ctx`). Scalars, Optional/Union (smart
  mode, with Pydantic's loc labels), Literal, Enum, list/set/frozenset/tuple/dict, dataclasses, datetime/date/time,
  UUID, Decimal, bytes, constraints (gt ge lt le multiple_of min_length max_length pattern, in Pydantic's order),
  Annotated metadata from httpp's Field, pydantic's Field and annotated_types, a real pydantic model (delegated,
  errors translated). Helpers: `is_scalar_hint`, `sequence_item_hint`, `constraints_from_metadata`.
- `src/bindings/python/httpp/models.py`: `BaseModel`, `Field`/`FieldInfo`, `ValidationError`, `ConfigDict`,
  `field_validator`, `model_validator` (before/after), `to_jsonable` (what FastAPI's jsonable_encoder does),
  `model_validate`, `model_validate_json`, `model_dump` (mode, include, exclude, by_alias, exclude_none,
  exclude_unset, exclude_defaults), `model_dump_json`, `model_copy`, `model_fields`, `model_fields_set`, v1 names
  (`dict`, `json`, `parse_obj`, `from_orm`, `copy`), config `extra` (ignore/forbid/allow), `populate_by_name`,
  `from_attributes`, `str_strip_whitespace`, self-referencing and function-local models (the defining frame's
  names are kept). `model_json_schema()` raises `NotImplementedError` (not written).
- `tests/python/test_validate_vs_pydantic.py` (242 cases) and `tests/python/test_models_vs_pydantic.py` (36
  cases): the same inputs through Pydantic (the oracle, `pip install fastapi pydantic httpx`) and through httpp,
  comparing value or error type/loc/msg/ctx/input. Error text of date/time/UUID parsing is compared by `type`
  only (`LOOSE` set). **All 278 passed, but only against a scratch copy of the two modules
  (`/tmp/devpkg`), NOT through the installed package, and NOT inside a full-suite run.** First thing to do next:
  rebuild (`ninja -C build-py`), then run the whole Python suite.
- `tools/fastapi_probes/`: three scripts that print what real FastAPI 0.143 / Pydantic 2.14 do. Re-run them
  instead of trusting memory (versions are newer than my training).

### Not done (in the order I would do it)

1. `httpp/params.py`: `Query`, `Path`, `Header` (underscores -> hyphens), `Cookie`, `Body` (`embed`), `Form`
   (urlencoded only), `Depends(dep, use_cache=True)` (+ our extra `blocking=True`), `HTTPException(status, detail,
   headers)`, `RequestValidationError`. They subclass `FieldInfo` (`Field` accepts the same constraint names).
2. A planner replacing `server.py::_prepare`: per handler parameter decide the source once at registration
   (path / query / header / cookie / body / form / dependency / `request` / `response` / `BackgroundTasks`), then a
   per-request resolver that collects ALL errors into one 422. **Keep the fast path:** a handler with no
   parameters returns `_NO_ARGS`; only path params / only `request` must stay as cheap as today (re-run
   `benchmarks/bench.py` and compare; the numbers in this file are the reference).
3. Default exception handlers for `HTTPException` and `RequestValidationError`, overridable with
   `@app.exception_handler(...)`; 422 body `{"detail": [errors]}` (FastAPI shape). This CHANGES the current
   path-parameter 422 body (`{"error": ...}`): update `tests/python/test_server_features.py` when doing it.
4. Responses: `Response`, `JSONResponse`, `PlainTextResponse`, `HTMLResponse`, `RedirectResponse`,
   `StreamingResponse`, `FileResponse`; injected `response: Response` (status/headers merged into the result);
   decorator options `status_code`, `response_model` (also from the return annotation; a mismatch is a 500),
   `response_model_exclude_unset/none/include/exclude/by_alias`, `dependencies`, `tags`, `summary`, `description`,
   `responses`, `deprecated`, `name`, `operation_id`, `include_in_schema`, `response_class`. `_to_response` must
   serialize models / dataclasses / Enum / datetime / UUID through `models.to_jsonable`.
5. Dependencies: sync, async, generator (teardown in reverse order, after the handler, the exception is thrown into
   the generator), async generator, class dependencies (`Depends()` with no argument uses the annotation),
   sub-dependencies, per-request cache, `app.dependency_overrides`. A sync dependency inside an `async def` route
   must go to the executor (never block the shared loop) unless `blocking=False`; async dependency inside a `def`
   route bridges through `_call`.
6. `BackgroundTasks` (run right after the response is produced; document that they are not strictly "after
   sent"), `APIRouter` + `app.include_router(router, prefix, tags, dependencies)`, `httpp.fastapi` namespace
   (`FastAPI`, `APIRouter`, `Depends`, `HTTPException`, `Query`, `Path`, `Header`, `Cookie`, `Body`, `Request`,
   `Response`, ..., `status`) so `from httpp.fastapi import ...` works, maybe an opt-in `sys.modules` shim.
7. `_schema.py` for `model_json_schema` and `/openapi.json` + `/docs`; `app.middleware("http")`; CORS;
   `{name:path}` path converters (needs a trailing-wildcard in the C++ router); multipart forms and uploads.
8. Differential tests for all of the above against real FastAPI (`TestClient`, compare status, JSON and headers).

### What the FastAPI oracle showed (so you don't re-probe; versions: fastapi 0.143, pydantic 2.14, starlette 1.8)

- A parameter with no marker: name in the path -> path; scalar (or Optional/Literal/Enum) -> **query**; anything
  else (model, dict, `List[int]`, dataclass) -> **body**. `user_agent: str` without `Header()` is a QUERY param.
  `List[int]` is a query param only with `Query()`.
- Query given twice for a scalar: the **last** value wins. Missing: `{"type":"missing","loc":["query","a"],
  "msg":"Field required","input":null}`. `?a=` is `int_parsing` with input `""`.
- Errors of all parameters (path, query, header, cookie, body) come back together in one 422
  `{"detail":[...]}`; a dependency that is shared adds its error once per use (the same error appeared 2-3 times).
- Body: one body param -> the whole JSON is the model; several body params, or `Body(embed=True)` -> object
  with one key per param; a body that is not an object counts as empty (each missing: loc `["body","name"]`);
  no body at all: `missing`, loc `["body"]`, input null. Invalid JSON: `{"type":"json_invalid","loc":["body",
  <char position>],"msg":"JSON decode error","input":{},"ctx":{"error":"Expecting value"}}`. A non-JSON
  `Content-Type` hands the raw body text to the validator (`model_attributes_type`, "Input should be a valid
  dictionary or object to extract fields from"); no `Content-Type` at all: it tries to parse JSON.
- `HTTPException` -> `{"detail": detail}` (detail may be a dict) + its headers. 404/405 -> `{"detail":"Not Found"}`
  / `{"detail":"Method Not Allowed"}`. `response_model` filters unknown keys and converts (`price` 1 -> 1.0); a
  mismatch is a plain 500. `status_code=` on the decorator is the default status. Returning `None` -> JSON `null`.
  `JSONResponse` keeps its own status and headers; `RedirectResponse` is 307 with `location`.
- Dependencies: a yield dependency used twice in one request is created once (cache) and torn down once, after
  the handler; order seen: `a-start, b, c, handler, a-end`. If the handler (or a later dependency) raises
  `HTTPException`, the teardown still runs. `dependency_overrides[original] = replacement` also replaces it for
  sub-dependencies. A router's `dependencies=` run before the route's. `Depends()` on a class calls the class with
  its `__init__` parameters resolved (`k: str = "kv"` becomes a query param).
- Injected `response: Response` -> its `status_code` and headers end up on the result (202 + `x-one` seen).

### Cautions

- Nothing of items 1-8 is wired into `server.py`: `Server.route` still uses the old `_prepare`; the new modules
  are standalone and only the tests import them. Nothing here changed the C++ side.
- Not benchmarked: the new layer. Do not accept a slowdown of the simple routes in section "hot path".
- `models.py` keeps the local names of the frame that defines a model (for forward references), like Pydantic;
  that keeps those objects alive as long as the class.
- `_validate.py` has no strict mode, no custom types beyond the list above, no generics.

## Update: hot path rebuilt (B + C), measured

What changed (all with tests first):

- **C++ API:** `server::route_io(method, path, io_handler)` runs the handler **on the I/O thread** and
  `server::route_shared(...)` is a normal worker route whose handler gets `std::shared_ptr<const request>`
  (so it can keep the request without a copy). `io_handler` must return at once and never block.
- **I/O-thread dispatch** (`server.cpp::on_request`): when the app has io routes and no `before` hook, the
  request is parsed and matched right on the I/O thread; an io route runs there (no pool hop), everything
  else goes to the pool already parsed and matched. A mounted directory that may hold the file, or a `before`
  hook, still takes the old worker path first. If an io handler answers on the spot and after/error hooks
  exist, `make_sender` moves the hooks to a worker (they may be Python and wait for the GIL).
- **One timer for all waiting requests** (`server::impl::waiting`): the per-request 504 timer was the biggest
  cost found (profile: ~1/3 of the I/O thread's active time; without it async went 13k → 33k). Requests that
  wait for a `route_async`/`route_io` answer now sit in an arrival-ordered map and one sweep timer expires
  them. Entries are dropped when answered (`responder::impl::settle`), so nothing keeps a request alive.
  The timeout is meant to be set before listening (deadlines are assumed to come in order).
- **Python:** `RawRequest` (Cython, shares the C++ request, reads fields on demand), `ServerRequest._from_raw`
  (lazy, read-only fields), `_prepare` works out once what the handler asks for and builds only that, one
  task per `async def` request (the timeout is a `call_later` that cancels it), the `done` call is direct
  unless after/error hooks exist. `Server.add_loop_route` is now a thin shim over `_add_raw_loop_route`.
  Hooks (`before_request`, `error_handler`) still receive the six copied values through `_FlatRaw`.
- **Benchmark:** `benchmarks/bench.py` (needs `oha`; `pip install uvicorn httptools uvloop`). Single core,
  server and `oha` share it, 32 connections, 4 s x 3, median req/s, same run:

| scenario | uvicorn (bare ASGI) | httpp `def` | httpp `async def` |
|---|---|---|---|
| hello | 43.2k | 62.8k | 64.4k |
| item (path param, JSON) | 38.3k | 52.9k | 51.0k |
| agent (reads a header) | 40.7k | 54.4k | 54.0k |
| many (last of 300 routes) | 41.8k | 61.3k | 59.5k |

  (Final run, after the router index below. An earlier run of the same code before the router had `def` at
  44.8k and `async def` at 57.6k with uvicorn at 41.1k; an A/B on one binary showed the router is not what
  moved `def` for hello, and I could not explain that jump: this machine drifts 10-30% between runs.)
  Before this work, same tool and machine: `def` 41.7k, `async def` 22.5k, uvicorn 41.9k. The machine is noisy
  (uvicorn varied 33k-42k between runs), so only same-run ratios mean anything. **Still a single core with
  the load generator on it: this is total CPU per request, not the multi-core story.** No multi-core number.
- **Router index** (`server.cpp::route_index`): routes made only of literal segments are found by one hash
  lookup, the rest by scanning only the parametric routes with the same method and segment count, always
  "the first route in registration order that fits". Characterization tests (incl. 250 random cases against
  the old linear algorithm) are in `tests/cpp/test_server_routing.cpp`. A/B on one binary: with 300 routes
  and a request to the last one, +20% (`def`) / +26% (`async def`); lookup time is now flat in the number of routes.
- **`--workers auto`** (`httpp run --workers auto`, `run(workers="auto")`): one worker per CPU the process can use
  (affinity mask, capped by a cgroup v1/v2 CPU quota, rounded down, min 1); one process off Linux or when
  `app` is an object.
- **README** repaired: the table of contents listed sections that `db16384` removed (client docs, CLI, C++,
  CMake, examples); it now lists what exists, the stray code fence and misplaced build paragraph are fixed.
  **Not restored:** the removed sections are still in `git show 9521d6a:README.md`; restoring them is the
  owner's call.
- **Why async is now faster than `def`:** an io route skips the pool hop that `def` still pays. Making `def`
  cheaper is a separate, open question (it must run on a worker because it may block).
- **Tests:** C++ 201 (was 179), Python 174 passed + 1 skipped (was 155 + 1). New files:
  `tests/cpp/test_server_io_routes.cpp`, `tests/cpp/test_server_routing.cpp`, `tests/python/test_server_raw_request.py`.
- **ThreadSanitizer** (build with `-fsanitize=thread`, `-DHTTPP_BUILD_PYTHON=OFF`): server, io-route and
  engine suites are clean. **Open finding, not from this work:** the `async_pool` suite (TLS client) reports
  data races inside **mbedtls** (`mbedtls_mpi_mul_mod`, `mbedtls_ssl_free`): mbedtls is built without
  `MBEDTLS_THREADING_C`, and the client pool uses it from several threads. Needs a decision (enable
  threading support with pthreads, and what to do on Windows) before the TLS client is called thread-safe.
- **Wheels:** `wheels.yml` has a second step for a `cp313-abi3` wheel (`wheel.py-api=cp313`), uploaded with
  the others. The generated C++ was checked to compile with `Py_LIMITED_API` for 3.9; the 3.13 floor can only
  be checked in CI (this sandbox has 3.12 headers). Free-threaded wheels stay as they are. `release_template.md`
  still lists per-version wheels and is stale.

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

Request paths today (after the hot-path work; section 5 holds the older numbers):

- **Sync `def` route:** Asio loop thread (parse + route match) → C++ worker pool thread → takes the GIL → runs
  `_prepare` + handler → `done`. One thread hop. (If the app has no `async def` route and no io route, the
  worker parses the request itself, as before.)
- **`async def` route:** Asio loop thread (parse + match + `route_io` handler) → `inbox_push` (no GIL, no copy;
  a wake byte only when the queue goes empty→non-empty) → the single asyncio loop thread wakes, takes the GIL
  once per burst, `_drain_inbox` → `create_task` → handler → `done(...)` called directly on the loop thread.
  One thread hop. With a `before_request` hook it goes through the pool first (two hops) because that hook
  may block.
- If the app has any `after_request` or `error_handler`, `done` goes through `run_in_executor` instead,
  because those hooks run *inside* `done()` on the calling thread and an `async def` hook would wait on the
  very loop that called it (deadlock; proven by forcing the direct path). `# ponytail:` marked; upgrade path:
  await the hooks inside the coroutine.
- Python request objects: handlers get a `RawRequest` shared with C++; `ServerRequest` is built only if the
  handler takes `request` (or an exception handler runs), and its fields are read on first use.

## 4. Build and test

```sh
pip install -r requirements-dev.txt
pip install cmake ninja --break-system-packages     # sandbox: they land in /usr/local/bin
export PATH=$PATH:/usr/local/bin:$HOME/.local/bin
cmake -B build -G Ninja -DHTTPP_BUILD_TESTS=ON -DHTTPP_BUILD_PYTHON=OFF && ninja -C build
build/tests/cpp/test_httpp_cpp                      # 201 tests

cmake -B build-py -G Ninja -DHTTPP_BUILD_PYTHON=ON -DHTTPP_BUILD_TESTS=OFF && ninja -C build-py
PYTHONPATH=build-py/python/install python3 -m pytest tests/python -q     # 174 passed, 1 skipped
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

1. ~~Remove one thread hop for `async def`~~ **done** (see "Update: hot path rebuilt").
2. ~~Build only what the handler asks for~~ **done**. Possible follow-ups, not started: moving `before_request` onto the
   fast path, a cheaper `def` path, coalescing the inbox wake byte per I/O batch, response building in C++.
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
    for the loop inbox are written but never run), ASan on server and engine (ThreadSanitizer: done for server/engine, see the update), running the
    new `wheels.yml` matrix, the mbedtls threading finding.
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


---

## Update: validators moved to Cython (this session)

- New `src/bindings/python/_validate_cy.pyx` (extension `httpp._validate_cy`, wired in `src/bindings/python/CMakeLists.txt`): every validator is a `cdef class` with a C-level `_validate(v, loc, errors)`, and the model field loop (`ModelV`) lives there too.
- Two passes, one code path: `errors is None` = fast pass (no loc tuples, no messages; first failure returns INVALID); only if it fails, the same validator runs again with `errors` to build the Pydantic-shaped errors. Consequence: a user `@field_validator` can run twice on an *invalid* input, so it must not have side effects (documented in the pyx docstring).
- `_validate.py` now only walks type hints (once, at registration) and builds those objects; `models.py` keeps schema building, `FieldInfo`, `ValidationError`, hooks. `inspect.signature` for hooks is cached.
- Baseline before the change: 452 passed, 1 skipped (the 278 Pydantic differential tests ran against the *installed* package, which closes that open item). After: same 452 passed, 1 skipped.
- `benchmarks/bench_validate.py` (median ns, same machine, same run order): User model valid 10759 -> 3398; List[int] x10 1361 -> 584; Union[int,str,None] 954 -> 159; User model invalid (5 errors) 17280 -> 11305. Scalar rows (~130-200 ns) are dominated by the Python lambda and the public two-pass `__call__`, not by validation. The machine is noisy: only compare numbers from the same run.
- Not done: wiring `models.py`/`_validate.py` into `server.py`, `params.py` (Query/Path/Header/Depends), planner, exception handlers, responses, OpenAPI; sub-interpreter experiment; `app.shared`; TSAN finding in mbedtls still undecided.


---

## Update: Swagger / OpenAPI (this session)

- `/openapi.json` (OpenAPI 3.1) and `/docs` (Swagger UI 5.33.2, **vendored** under `src/third_party/swagger-ui/`, Apache-2.0, nothing from a CDN; see the next section: it is compiled into `libhttpp`, no data files are shipped). `Server(title=, version=, description=, openapi_url="/openapi.json", docs_url="/docs")`; `docs_url=None` / `openapi_url=None` switch them off. `app.openapi()` returns the document (cached, rebuilt when a route is added). No ReDoc (it would be a second ~1 MB bundle).
- `_params.py` (new): where each handler argument comes from, worked out at registration with FastAPI's rules: name in the path = path param (`{x}`, `<x>`, `:x`); scalar or list of scalars = query; anything else = JSON body (one body param only). The same plan drives the request path (`server.py`, 422 `{"detail": [...]}` for query/body/non-int path errors) and the OpenAPI document, so docs and behaviour cannot drift. Legacy int/float path-param 422 (`{"error": ...}`) is kept because a test pins it.
- `_schema.py` (new, plain Python, runs once): `model_json_schema()` (Pydantic v2's shape; compared with Pydantic's output in `test_openapi.py`, equal on a model with ~20 field kinds) and the document builder.
- Per-route options: `summary, description, tags, deprecated, include_in_schema, status_code, response_model` (`response_model` validates + serializes the return value; a mismatch raises `ResponseValidationError` -> 500). A returned model / dataclass is now serialized as JSON.
- Tests: `tests/python/test_openapi.py` (18; one validates the document with `openapi-spec-validator` and skips if it is not installed). Whole suite: 470 passed, 1 skipped.
- Not done / limits: `Field()`/`Query()`/`Header()`/`Cookie()`/`Depends()` as parameter defaults (a `Field(...)` default raises TypeError on purpose), several body params / `embed`, form and file bodies, security schemes, `servers`, response `examples`. `/docs` was opened in headless Chromium (Playwright): the page renders from local files only (no external requests, no JS errors), lists both operations and the schemas, and Try-it-out worked (GET 200 with converted query values, POST 422 with `detail`, POST 201 through `response_model`). The C++ side has no docs generator (not needed yet).
- abi3: `_validate_cy.pyx` compiles with `-DPy_LIMITED_API=0x030900f0 -DCYTHON_LIMITED_API=1` and a smoke test imports and runs, but the full cp39-abi3 / cp313-abi3 wheel build was not run.


---

## Update: Query / Path / Header / Cookie / Body / Depends (this session)

- `from httpp import Query, Path, Header, Cookie, Body, Depends`. All but `Depends` are `ParamInfo(FieldInfo)` (in `_params.py`): constraints like `Field()`, plus `alias`, `description`, `examples`, `deprecated`, `default_factory`; `Header(convert_underscores=True)` reads `user_agent` from `User-Agent` (case-insensitive); `Body(embed=True)`. They work as a default (`q: str = Query(min_length=3)`) and inside `Annotated[int, Query(ge=1)]`. A plain `Field()` default on a handler argument is still a TypeError on purpose.
- Several body parameters (or `embed=True`) are read from a JSON object by name, like FastAPI; one body parameter is the whole body.
- `Depends(fn)` / `Annotated[T, Depends(fn)]` / `Depends()` (class from the annotation): dependencies take their own path / query / header / cookie / body parameters, `request`, and other dependencies; result cached per request (`use_cache=False` to run again; a failed one is cached as failed so its errors are reported once); `yield` dependencies (sync and async) are closed by an ExitStack after the handler, also when it raised; an exception from a dependency goes to the registered exception handler. Errors of all dependencies are collected into one 422.
- A sync route can't use an `async def` dependency (TypeError at registration). In an `async def` route a *sync* dependency runs on the event-loop thread (FastAPI uses a thread pool): keep those cheap or make them async.
- OpenAPI: parameters of the route and of every dependency below it (deduplicated), `in: header/cookie`, header names with hyphens, constraints / description / deprecated / examples, embedded bodies as one object schema (`Body_<operationId>`, inline, not under components).
- Known limits: if the route's own parameters are invalid, its dependencies are not run (so only those errors are reported); `yield` cleanup runs after the handler returns but *before* the response is written (FastAPI's order differs slightly); `security_scopes` / `Security()` and forms / files are not done; Swagger's Cookie parameter can't be sent from a browser (it is shown, the browser refuses to set `Cookie`).
- Checked in headless Chromium: header / cookie / query / dependency parameters show up in `/docs` (required marker, constraints; Swagger itself blocks `n=99` because the document says `maximum: 10`), Try-it-out works for GET with a dependency (200) and for an embedded two-parameter body (200), no JS errors, no external requests.
- Tests: `tests/python/test_params_depends.py` (21) + the OpenAPI ones; whole suite 491 passed, 1 skipped.


---

## Update: Swagger UI compiled into the library at configure time (this session)

Modelled on liburlparser's `cmake/EmbedFile.cmake` (which turns `public_suffix_list.dat` into a header at configure time).

- `cmake/EmbedFile.cmake` (`embed_files(OUTPUT ... NAMESPACE ... FILES name path ...)`) + `cmake/utils/embedded_files.in.hpp`: at `cmake -B build` every file is read as HEX and written into ONE generated header (`build/generated/httpp_swagger_ui_data.hpp`, ~7.5 MB) as `inline constexpr unsigned char <name>[] = "\x..\x.." "..."; ` plus `<name>_size`, via `configure_file` (header untouched when nothing changed, so no rebuild) and `CMAKE_CONFIGURE_DEPENDS` (a changed source file re-runs CMake).
- Differences from liburlparser's version, on purpose: (1) a literal per **32 bytes**, not per source line: `swagger-ui-bundle.js` is a single 1.5 MB line, so per-line would give one huge literal; (2) every byte is a `\xNN` escape: binary-safe (the PNG has NULs), no source-charset / trigraph / `\x`-swallows-next-digit surprises; (3) `constexpr unsigned char[]` in `.rodata` instead of `inline const std::string` (a std::string would copy 1.6 MB onto the heap in every process at start-up); (4) `-DHTTPP_EMBED_AS_ARRAY=ON` (default ON for MSVC) emits a byte array instead, because MSVC limits a concatenated literal. **The array form is only tested with GCC**; on MSVC the arrays of 1.6 MB are slow to compile (GCC: ~2 s vs 0.06 s for strings) and untested.
- C++ API: `include/httpp/swagger_ui.hpp` -> `httpp::find_swagger_ui_asset(name)` -> `{name, content_type, data}` (string_views into .rodata); `src/core/swagger_ui.cpp`. Python: `httpp_cy._swagger_ui_asset(name)` -> `(bytes, content_type)`, cached once per process in `server.py`; the Python package carries no `static/` directory any more, and the wheel / install tree has no .js/.css/.png.
- Sources live in `src/third_party/swagger-ui/` (see its `VENDORED.md` for how to bump the version).
- Tests: `tests/python/test_embed_file.py` (nine awkward files: 0, 1, 31, 32, 33, 64 bytes, all 256 byte values, text with `"`, `\`, `??/`, `@NAME@`, `${y}`, NULs, and a 320 KB single line; in string and array form; plus "unchanged input does not touch the header"), `tests/cpp/test_swagger_ui.cpp` (compiled-in bytes == files on disk), and `test_openapi.py` (served bytes + content types == the vendored files). Python suite: 494 passed, 1 skipped. The C++ test was compiled by hand against the built library (4 tests pass); the full `HTTPP_BUILD_TESTS=ON` C++ suite was not re-run this session.
- Not done: the C++ server has no ready-made "mount /docs" helper (only the assets); the HTML page is still built in Python (`server.py`).
