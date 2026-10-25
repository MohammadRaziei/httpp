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
1. body deadline (done) 2. GZip (done) 3. router (done) 4. `response` injection and `after_send` (done) 5. forms and uploads (done) 6. `ServerRequest` parity.
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

## Rejected alternatives
- Middleware stack as in Starlette: flexible, but a per-request cost on every route and a different C++ and Python model.
- Python-only implementation of the new features: faster to write, breaks the dual rule, and puts the cost on the GIL.

## Not verified
Nothing here is measured yet except what each step reports. Windows and macOS runtime stay as in hand-off #6.

## Related
ADR-0016, ADR-0017, ADR-0020, ADR-0021
