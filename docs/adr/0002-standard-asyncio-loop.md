# ADR-0002: `async def` handlers run on the standard asyncio loop

- **Status:** accepted
- **Decided by:** the project owner (after discussion)
- **Source:** `Architecture.md` section 6, `handoff.md` sections 2 and 3

## Context

Network I/O is done by Asio in C++, but `async def` handlers must work with asyncio libraries (httpx, asyncpg, anyio).
An event loop for user code has to be chosen.

## Decision

One **standard asyncio loop** on its own thread. Asio parses and routes the request, puts it in a queue
(`inbox`) and writes one wake-up byte to the loop's socket only when the queue goes from empty to non-empty. The loop takes the GIL once,
drains the queue, calls `create_task` for each request and calls `done(...)` directly on the same thread.

## Rejected alternatives

- **An asyncio loop built on Asio (uvloop-style):** far too heavy.
- **A custom coroutine runner in C++:** it would only understand our own awaitables and be incompatible with httpx/asyncpg/anyio.

## Consequences

- All `async def` handlers share one thread and one GIL; real parallelism comes from `--workers` or free-threaded Python.
- Wherever an `after_request` or `error_handler` exists, `done` goes through `run_in_executor`, because these hooks run inside `done()`
  and an `async def` hook would wait on the same loop that called it (deadlock; proven by forcing the direct path).
  The code carries a `ponytail:` mark; upgrade path: await the hooks inside the coroutine itself.
- A synchronous dependency inside an `async def` route runs on the loop thread (FastAPI uses a thread pool).

## Related

ADR-0001, ADR-0009
