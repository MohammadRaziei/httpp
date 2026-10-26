# ADR-0022: Dual C++/Python rule and the decisions the owner delegated

## Status
Accepted. On 2026-10-24 the owner delegated every open decision ("decide yourself") and set one architecture constraint.
This delegation covers the items listed below only; it is not a standing permission for other critical decisions.

## Context
Hand-off #6 listed open work that needed an owner decision: `response: Response` injection, `background=`, a full `Request`,
`APIRouter`, `BackgroundTasks`, middleware, forms and uploads, GZip, a total deadline for a slowly dripped body, `--workers N` as threads.
ADR-0016/0017 said the FastAPI-like layer is "Python by design" and that Python binds only what is necessary.

## The constraint (owner)
1. **Dual API.** A feature exists in C++ and in Python, written the same way. Python may differ only where the GIL forces it
   (the asyncio bridge, how handlers are scheduled). Everything else is identical, as far as possible.
2. **Fastest implementation, no bottleneck.** Logic lives in the C++ core; Python binds it. Nothing new may add a per-request cost to a
   route that does not use the feature.
3. **Clean OOP.** One class per concept, small public surface, no feature flags scattered over the engine.

## Decision
- **Where logic lives.** New features are written in the C++ core with a public C++ API; the Python API is a thin binding with the same names.
  This replaces "Python by design" for *new* work. The existing Python-only layer (OpenAPI, models, `Depends`, `run()` and the CLI) is not
  rewritten now; it stays listed as debt in `checklist.md`.
- **FastAPI compatibility level (Architecture risk 4): FastAPI-like, natively.** The Python front keeps FastAPI's names and shapes
  (decorators, `Query/Path/Header/Cookie/Body/Depends`, `HTTPException`, response classes, `response_model`, OpenAPI and `/docs`, `APIRouter`,
  `BackgroundTasks`), so most FastAPI code moves by changing the import. It is not drop-in: no ASGI, our own Pydantic-shaped models
  (ADR-0006), no Starlette middleware stack. Anything FastAPI has that costs speed on every request is left out on purpose.
- **Body deadline** (done): `server::set_body_timeout(seconds)`, `Server(body_timeout=...)`. The deadline counts from the end of the head,
  default 120 s (a slow upload of a big file must still fit; set it higher for that), `0` = none. Same mechanism as `header_timeout`.
- **No ASGI-style middleware stack.** A stack wraps every request in extra calls and, in Python, extra frames. The hooks that exist
  (`before`, `after`, error and exception handlers) are the middleware, in both languages. Features that people write middleware for
  become core features: compression is `server::set_compression(min_size)` (the vendored deflate, no new dependency).
- **Response injection.** C++ handlers already receive a mutable `response&`. A Python handler may ask for `response: Response` by name and
  gets the same object shape (status, headers, cookies); its return value stays the body. Handlers that do not ask pay nothing.
- **Background work.** One mechanism in the core: `response::after_send`, a job the server runs on the worker pool after the answer was
  written. It replaces the `server::defer(fn)` first planned: tied to the answer it needs no access to the server from a handler, and it
  runs exactly once however the answer ends. Python's `BackgroundTasks` and `response.after_send` use it. Handlers that do not use it pay nothing.
- **Router.** C++ class `httpp::router` (prefix, routes, nested include) and `server::include(router)`; Python `APIRouter` binds it.
  Prefixes are joined at registration time, so a request costs the same as without a router.
- **Forms and uploads.** A lazy multipart / urlencoded parser in C++ (`request::form()` and `request::files()`), bound in Python.
  It runs only when the handler asks, never for other routes.
- **Full `Request`.** The Python `ServerRequest` gets the same fields as the C++ `request` (it mostly has them); `form` and `files` follow the form work.
- **`--workers N` as threads: not done.** Free-threaded Python already runs handlers on the pool in one process (hand-off #6 stress: 74k to 77k req/s
  on 3.14t against 51k on 3.13). A second thread model would duplicate the pool without a measured gain. Reopen with a measurement on more than 2 cores.

## Order of work
1. body deadline (done) 2. GZip (done) 3. router (done) 4. `response` injection and `after_send` (done) 5. forms and uploads (done) 6. `ServerRequest` parity (done).
Each step: tests first, C++ and Python together, ADR note if the design changes, zip.

## Notes from finished steps
- **GZip.** `httpp::detail::response_compression` (one class, `src/core/response_compression.cpp`), called once in the server's sender after
  the hooks. Cost when off: one inline check. Measured with the vendored stb deflate (one run, 2 cores, 20 KB of repetitive JSON-like text):
  level 1 takes 135 us and gives 937 bytes, level 6 takes 173 us and gives 866 bytes, so level 1 is the default. That is a few
  times slower than zlib at the same level (not measured here), the price of zero system dependencies; `min_size` and the level are
  the two knobs. Compression runs on the thread that answers (a pool thread for `def` handlers; for `async def` most likely the Python asyncio loop thread, not verified,
  where it holds up the loop for those 100+ us). ponytail: no offload of big async answers to the pool; upgrade path: send them through
  `blocking_pool` when a profile shows it.
- **Router.** C++ `router` is a list of (method, path, handler) with a prefix; `server::include` replays it. Python `APIRouter` is
  written in Python and replays into `Server.route` instead of binding the C++ class: a Python route must be adapted (parameters,
  models, dependencies, `def` or `async def`) at registration, which C++ knows nothing about. Behavior is the same and is tested the
  same way in both. A wrong HTTP method is reported when the router is included, in both languages. `route_io` and `route_shared`
  are not in Python (same reason as on the server).

- **after_send / response injection.** `wire_response::after_send` is kept by the connection and handed to the pool when the answer is
  written, or when the connection closes while writing it (a `shared_ptr` whose deleter enqueues the job, so it cannot run twice or be
  forgotten). It is dropped when the server is being destroyed. In Python a list subclass carries the job to `_fill_response`
  (one `type(headers) is not list` check per answer on the plain path). `response.status`, when set, wins over a returned status. Not done:
  injection into dependencies, and `background=` as a keyword of `Response` classes (use the argument).
- **Forms.** `request::form()` is an exported member (the struct stays otherwise header-only); it returns `form_data` by value and
  parses on every call, so the Python `ServerRequest` caches it. Limits: 1024 parts, 8 KiB of headers per part, boundary up to 200 bytes;
  the body is already whole in memory because the engine reads it before the handler runs. Only `\"` is an escape inside a quoted
  parameter (browsers send Windows paths with raw backslashes). Not done: streaming an upload to disk without holding it in memory
  (needs the engine to stream request bodies, a bigger change), `filename*=` (RFC 5987) and per-part charsets.
- **ServerRequest parity.** C++ `request` has `header`, `query_param`, `path_param`, `cookie` and `form`; Python now has all five. Two
  differences remain on purpose: Python `headers`, `query` and `path_params` are first-wins dicts (C++ keeps every repeat, in order), and
  `body` is `bytes` with `text` and `json()` helpers.
- **Measured after the six steps (2026-10-25).** C++ suite (287 tests) under ThreadSanitizer: all pass, no report. `benchmarks/bench.py`,
  `oha`, 2 cores shared with the load generator, Python 3.13, 1 worker, 32 connections, 2 rounds of 4 s, median req/s, no gzip, no hooks:
  `hello` httpp `def` 52.8k, `async def` 49.1k, uvicorn (bare ASGI, uvloop, httptools) 34.5k, FastAPI + uvicorn 7.9k;
  `item` (int path parameter, JSON) 50.3k, 44.9k, 24.2k, 6.4k. One run, rounds within 3 percent of each other; compare only inside the table.
  uvicorn's `item` is a bare ASGI app with no routing or validation, so FastAPI is the like-for-like row.
- **Why `async def` is about 10 to 15 percent behind `def` (investigated 2026-10-25; 2 cores shared with `oha`, 3.13, `/async` and `/`, 32
  connections; script medians of 5 interleaved rounds of 5 s, so treat differences under about 5 percent as noise).** Server CPU per request:
  `def` about 17 to 18 us (52k req/s), `async def` about 22 to 23.5 us (43 to 45k req/s); the machine is CPU bound because the load generator
  takes the other core, so every extra us per request shows up as lost throughput. Where the extra 4 to 5 us is:
  (a) A loop route that answers from the dispatcher with no Task and no coroutine (`_add_raw_loop_route`, no tracking) costs 21.4 us:
  the whole Python asyncio part (Task, `run`, deadlines, `_to_response`) is only about 2 us (under 10 percent). (b) The C++ 504 backstop
  (`track`, a mutex and a map entry per request) costs about 0 to 3 percent: removing it (`handler_timeout=None`) gave 45.4k against 43.9k, inside the noise.
  (c) The rest, about 4 us, is the design: the request is parsed and matched on the I/O thread, queued to the loop thread, run there,
  and the answer posted back, against a worker that does all of it for `def`. `py-spy --gil --native` shows 41 percent of the loop thread's
  GIL-held samples inside `free()` waiting on a glibc arena lock (memory made on the I/O thread is freed on the loop thread), but the GIL is
  held only about a quarter of the wall time, `MALLOC_ARENA_MAX=1` made it worse (40k) and a larger tcache did nothing (46k), so this is
  one more small cost and not a fix that is waiting to be applied. Syscalls per request are fewer than for `def` (2.6 against many futex waits).
  Not done, with the ceiling: skipping the Task for a handler that finishes without waiting (needs a manual coroutine driver that has to get
  cancellation right; best case about 2 us, 9 percent); batching answers (ADR-0021, about 2 us). Both stay open if `async def` throughput becomes the limit.
  `async def` is still 1.3 to 1.9 times uvicorn on the same box.

## Rejected alternatives
- Middleware stack as in Starlette: flexible, but a per-request cost on every route and a different C++ and Python model.
- Python-only implementation of the new features: faster to write, breaks the dual rule, and puts the cost on the GIL.

## Not verified
Nothing here is measured yet except what each step reports. Windows and macOS runtime stay as in hand-off #6.

## Related
ADR-0016, ADR-0017, ADR-0020, ADR-0021

## Answer batching (2026-10-26)

The owner said the `async def` gap is an architecture problem, not tuning. A pure C++ floor (no Python) agrees in part: answering from the I/O thread gave about 72k req/s, handing the answer to one other thread about 61k, so the thread hop alone costs around 15 percent. Answers now go through a batched poster in `async_server.cpp`: only the push that finds the queue empty posts a drain task to Asio. Measured with interleaved runs: C++ pool path +10 percent, `async def` +4 percent, `def` +4 percent (the last two are close to the noise). It helps both APIs because it lives in the core. The bigger step, running the `async def` handler without a hop, is still open.
