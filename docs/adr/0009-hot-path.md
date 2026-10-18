# ADR-0009: Hot path: dispatch on the I/O thread, one timer for everything, an indexed router

- **Status:** accepted
- **Source:** `handoff.md` ("hot path rebuilt"), `src/core/server.cpp`

## Decision

- `server::route_io(method, path, io_handler)` runs the handler **on the I/O thread** (it must return at once and never block).
  `route_shared` is an ordinary route on the worker pool that takes a `shared_ptr<const request>`. When the app has an `io` route and no
  `before` hook, the request is parsed and matched right there; everything else goes to the pool already parsed.
- **One timer** for all requests waiting for a response (instead of one 504 timer per request): a map ordered by arrival and one sweep.
  The per-request timer took about a third of the I/O thread's active time; without it `async` went from 13k to 33k.
- **Indexed router:** literal-only routes use one hash lookup; the others look only among parameterized routes with the same method and the same number
  of segments, always "the first route in registration order that matches". With 300 routes and a request for the last one: +20% (`def`) and +26% (`async`).
- Python: `RawRequest` (Cython, shared with C++, fields built lazily), and `ServerRequest` only if the handler asks for `request`.

## Consequences

- The `async def` path has one thread hop; `def` also has one (to the pool, because it may block).
- Numbers (one core, server and `oha` on the same core, 32 connections, median; only the ratios within one run mean anything):
  uvicorn 43.2k, httpp `def` 62.8k, `async def` 64.4k on `hello`. The machine fluctuated 10 to 30%.
  In the later benchmark on `item`: uvicorn 21.1k, `def` 25.2k, `async def` 24.9k; on `hello`: uvicorn 23.0k, `def` 29.5k, `async def` 33.4k.
  **There are no multi-core numbers.**
- The 504 timer must be set before `listen` (the deadlines are assumed to be ordered).
- In `def`, the hop to the pool remains; making it cheaper is an open question.

## Related

ADR-0002, ADR-0005
