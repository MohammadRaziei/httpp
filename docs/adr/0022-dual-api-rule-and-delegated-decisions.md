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
- **Body deadline** (done): `server::set_body_timeout(seconds)`, `Server(body_timeout=...)`. The deadline counts from the end of the head,
  default 120 s (a slow upload of a big file must still fit; set it higher for that), `0` = none. Same mechanism as `header_timeout`.
- **No ASGI-style middleware stack.** A stack wraps every request in extra calls and, in Python, extra frames. The hooks that exist
  (`before`, `after`, error and exception handlers) are the middleware, in both languages. Features that people write middleware for
  become core features: compression is `server::set_compression(min_size)` (the vendored deflate, no new dependency).
- **Response injection.** C++ handlers already receive a mutable `response&`. A Python handler may ask for `response: Response` by name and
  gets the same object shape (status, headers, cookies); its return value stays the body. Handlers that do not ask pay nothing.
- **Background work.** One mechanism in the core: `server::defer(fn)` runs `fn` on the worker pool after the answer was written.
  Python's `BackgroundTasks` and `background=` call it. Handlers that do not use it pay nothing.
- **Router.** C++ class `httpp::router` (prefix, routes, nested include) and `server::include(router)`; Python `APIRouter` binds it.
  Prefixes are joined at registration time, so a request costs the same as without a router.
- **Forms and uploads.** A lazy multipart / urlencoded parser in C++ (`request::form()` and `request::files()`), bound in Python.
  It runs only when the handler asks, never for other routes.
- **Full `Request`.** The Python `ServerRequest` gets the same fields as the C++ `request` (it mostly has them); `form` and `files` follow the form work.
- **`--workers N` as threads: not done.** Free-threaded Python already runs handlers on the pool in one process (hand-off #6 stress: 74k to 77k req/s
  on 3.14t against 51k on 3.13). A second thread model would duplicate the pool without a measured gain. Reopen with a measurement on more than 2 cores.

## Order of work
1. body deadline (done) 2. GZip 3. router 4. `response` injection and `defer` 5. forms and uploads 6. `ServerRequest` parity.
Each step: tests first, C++ and Python together, ADR note if the design changes, zip.

## Rejected alternatives
- Middleware stack as in Starlette: flexible, but a per-request cost on every route and a different C++ and Python model.
- Python-only implementation of the new features: faster to write, breaks the dual rule, and puts the cost on the GIL.

## Not verified
Nothing here is measured yet except what each step reports. Windows and macOS runtime stay as in hand-off #6.

## Related
ADR-0016, ADR-0017, ADR-0020, ADR-0021
