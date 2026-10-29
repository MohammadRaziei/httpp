# Hand-off 13

Read this first in the next session. It replaces handoff_12.md (older hand-offs stay for history; the working rules, sandbox notes, benchmark reference
numbers and the crash notes of handoff_12 still apply and are not repeated).

## State
- **WebSocket (RFC 6455), C++ core: done** (ADR-0026). Python binding: not started.
- C++: 366 tests pass (296 before + 42 in `test_websocket_session.cpp` + 28 in `test_server_websocket.cpp`). ThreadSanitizer (RelWithDebInfo, `-fsanitize=thread`)
  on both new files: 0 warnings. Not run under TSan: the rest of the suite. Not run under ASan.
- Python after this change: 625 pass (4 skipped) on 3.14 and 624 pass (5 skipped) on 3.14t. The parity test forced two entries: `("websocket.hpp", "websocket"): None` in TARGETS and
  `("server", "route_ws")` in NOT_EXPOSED; both must go when the Python binding exists.
- Build only `--target test_httpp_cpp`; use `--filter='httpp_server_ws*'` (glob, not a prefix) to run a group.

## What was built
- `src/core/internal/async_server.hpp`: `tunnel`, `tunnel_io`, `tunnel_post_fn`, `wire_response::upgrade`. A 101 answer with a tunnel turns the connection into a byte
  stream (engine: queue + one async write at a time, read loop with pause, `set_idle`, `set_deadline`, `close_after_flush`, write timeout, `on_close` exactly once).
  The 101 head is the first thing in the queue and `on_open` runs before it is written (so a client that saw the 101 knows `on_open` ran); the head is not counted in `tq_bytes`.
- `src/core/internal/websocket.hpp/.cpp`: accept key (PSA SHA-1, no new dependency), key check, strict UTF-8, close codes, frame writer, `ws_session` (parser, fragments,
  control frames, limits, keep-alive ping, close handshake, bounded queue with `full` and `on_drain` at half) and `ws_shared` (the thread-safe side that handles use).
- `src/core/websocket_api.cpp`: the public `httpp::websocket` methods. `include/httpp/websocket.hpp`: `websocket`, `ws_handler`. `server::route_ws(path, ws_handler)`.
- `server.cpp`: `route_entry::ws`, `run_ws` (handshake on the I/O thread, 400/426/403/500 rules, `on_handshake`, subprotocol check), ws routes count as io routes.
- The test target compiles `websocket.cpp` in (like `async_server.cpp`), because the internals are not exported.

## Decisions to remember (all in ADR-0026)
Handlers run on the I/O thread and must not block. Sends are thread-safe (direct on the connection's loop thread, posted otherwise). A refused send (`full`) is never a silent drop.
No `permessage-deflate`, no `wss` of its own (the server has no TLS). The `after` hook does not run for the 101. `on_close` is reported only if `on_open` ran. Server stop gives 1006.
UTF-8 of a text message is checked when the message is complete, not per fragment.

## Next
1. CI check (not looked at).
2. **Python binding** (ADR addendum first): `Server.websocket("/path")` decorator or `app.websocket`, an object with `await receive()` / `async for`, `send_text/send_bytes/close`, `accept`-less
   (the C++ handshake already accepted; `on_handshake` becomes an optional guard). Follow the lanes design (ADR-0024): the handler of a connection runs on the asyncio loop of the lane of its
   I/O thread; incoming messages go to a per-connection queue on that loop, bounded, and `pause_reading()` / `resume_reading()` are the backpressure to the peer; `send_*` returns/raises on `full`
   (decide: await until `on_drain`, as the natural `async` behaviour). With a GIL there is one loop. Check the free-threaded build.
3. Autobahn testsuite run (docker is not available in the sandbox: ask the owner to run it, or use `wstest` via pip in a venv). Expected gaps: per-fragment UTF-8 timing (cases 6.4.x) and
   `permessage-deflate` (cases 12 and 13, not supported by design).
4. WebSocket benchmark in `benchmarks/` (echo at 64 connections, messages/s and p99, against the C++ core, `websockets`, `uvicorn[standard]`); memory with many idle connections.
5. The items 1 to 7 of handoff_12 "Next" that are still open (next `make bench`, competitor benchmarks, the free-threaded pool default, README gaps).

## Not verified
Windows and macOS (the engine tunnel code uses the same Asio calls, but nothing was run there). Autobahn conformance. TSan on the rest of the suite (only the two WebSocket test files were run under it).
Behaviour with `wss` behind a TLS proxy.
