# ADR-0038: Graceful shutdown with a grace period

- **Status:** accepted
- **Source:** `server::shutdown`, `async_server::drain` and `impl::begin_drain` in `src/core/`, `Server.shutdown`, `httpp run --shutdown-timeout`

## Context

`server::stop()` closed the listener and every connection at once: a request that was being handled lost its answer. Ctrl+C and SIGTERM in `httpp run` used it, so a deploy or a restart
cut requests. The owner delegated the design (2026-11-06).

## Decision

1. `server::shutdown(grace = 10 s)` (C++) and `Server.shutdown(grace=10.0)` (Python, seconds) start a drain on every engine:
   the listener closes at once; connections that wait for the first byte of a request (idle keep-alive, just accepted) and tunnel (WebSocket) connections close at once;
   a connection with a request in progress is left alone and its answer is written with `Connection: close`; the last connection to close ends the loop.
   Each engine ends by itself (last connection gone, or its own grace period over); `listen_after_bind` no longer stops the others when engine 0 finishes first after a
   `shutdown()` (found by ThreadSanitizer runs: with several I/O threads an engine with no connections ended at once and cut the requests of the others).
   After `grace` whatever is still open is closed as `stop()` does. `stop()` stays the immediate stop and also ends a drain.
2. A request whose first bytes arrived before the drain but whose head or body is incomplete counts as in progress; the read, head and body timeouts and the grace period bound it.
3. `listen()`: Ctrl+C and SIGTERM now call `shutdown(app.shutdown_timeout)` (class attribute, default 10, set by `run(shutdown_timeout=...)` or `httpp run --shutdown-timeout`).
   A second Ctrl+C calls `stop()`. With `--workers`, the supervisor passes the option on and waits `grace + 2` seconds before it kills a worker (it waited a fixed 10 before).
4. Shutdown hooks (`on_shutdown`) still run after the server returns, as before.

## Rejected alternatives

- Change `stop()` to drain: callers (tests, tools) rely on it being immediate, and a drain needs a time limit.
- Send a `Connection: close` hint only, with no timer: one stuck handler would block shutdown for ever.
- Close WebSocket connections with a 1001 close frame: right in principle, but it needs a hook into the session layer; the tunnel interface only has `close()`.
  `ponytail:` WebSocket connections are closed abruptly at the start of the drain, as `stop()` always did. Upgrade path: send close 1001 and wait for the peer, inside the grace period.

## Consequences

- No cost when not shutting down: one `bool` read per answer (`srv.draining`, loop thread only).
- A synchronous handler that outlives the grace period is not interrupted (threads cannot be): the connection is cut, but `listen()` returns only after the worker pool has joined it, as before.
- Verified: 5 new C++ tests (in-flight answer with `Connection: close`, idle close, cut after grace, two I/O threads with a late answer, no-op when not listening), the whole C++ suite (428, Release),
  6 new Python tests including SIGTERM of a real `httpp run` process with a request in flight and a two-worker run, the whole Python suite on 3.13 (677 passed, 4 skipped).
  The whole C++ suite under ThreadSanitizer (Debug, `-O1`): 428 passed, no warnings; the shutdown tests 30 times each under TSan and Release with no failure.
  Not run: AddressSanitizer, sanitizers on the Python extension, Python 3.14 and 3.14t, Windows (SIGTERM semantics differ), macOS.
