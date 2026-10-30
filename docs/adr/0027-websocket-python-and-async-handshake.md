# ADR-0027: WebSocket in Python, the asynchronous handshake, and bounded mailboxes

## Status
Accepted (2026-10-30). The owner delegated every design choice ("implement the most principled and optimal way, the ADRs may be wrong").
This ADR amends [ADR-0026](0026-websocket.md): the handshake hook described there (a synchronous `on_handshake(request, response&)`)
is replaced by the asynchronous `ws_acceptor` below, and `Origin` handling changes in the Python layer.

## Context
ADR-0026 delivered WebSocket in the C++ core. Python needs the same thing with `async def` handlers, and three facts shaped the design:

1. A Python handler cannot answer the upgrade inside the I/O thread callback: it lives on an asyncio loop thread and needs the GIL. A
   synchronous hook would force the I/O thread to take the GIL (ADR-0024 forbids that) or to block.
2. Per-connection state must be reachable from the callbacks (all on the I/O thread) without a global map.
3. A slow Python handler must not make memory grow: receiving and sending both need a bound that pushes back on the peer.

## Decision

### C++: the handshake is answered later, from any thread
`ws_handler::on_handshake(std::shared_ptr<const request>, ws_acceptor)` runs on the I/O thread for an upgrade that passed the protocol
checks and returns at once. The answer comes from any thread: `acceptor.accept(headers, user)` or `acceptor.reject(...)`. The first call wins.
A dropped acceptor closes the connection without a response; an unanswered one gets a 504 after `set_async_timeout`, like `route_async`.
The engine does not read from a connection while its upgrade waits for an answer.
`accept` takes an opaque `std::shared_ptr<void> user`, returned by `websocket::user()` in every callback, so per-connection state needs no map.
`on_close` runs **exactly once for every accepted upgrade**, also when the connection ended before `on_open` (reported as 1006), because
the session's destructor reports it. Without a hook every valid upgrade is accepted immediately (unchanged).

### Python: one task per connection, a mailbox per connection
`@app.websocket(path)` registers an `async def` handler that takes `websocket` (by name or annotation). It is Starlette/FastAPI shaped:
`await websocket.accept(subprotocol, headers)`, `receive`/`receive_text`/`receive_bytes`/`receive_json`, `async for`, `send_text`/`send_bytes`/
`send_json`, `ping`, `close(code, reason)`, `WebSocketDisconnect(code, reason)`. Path and query parameters work as in `route`.
Handler returning before `accept` rejects with 403, an exception before it gives 500 (`HTTPException` its own status), an exception after
it closes with 1011, a normal return closes with 1000. Dependencies, `response` and background tasks are refused at registration (not supported yet).

The path of a connection, all without the I/O thread taking the GIL:

- The handshake callback creates a C++ `httpp_py_wsconn` (the mailbox), queues an upgrade item into the lane's existing inbox (the same
  inbox and wake socket as requests, ADR-0024) and returns. The loop thread starts the handler task, which calls `accept`.
- `on_message` on the I/O thread appends to the mailbox deque. A wake item is queued only if the handler is waiting (`want`) and none is
  already on its way (`armed`), so a burst of frames costs one wake.
- `receive()` pops from the mailbox on the loop thread. Check-and-arm is one step under the mailbox mutex, so a wake cannot be lost.
- The mailbox keeps the websocket handle only between open and close; the handle holds the mailbox as `user`, so the cycle is broken in `on_close`.
- Python keeps one reference to its `WebSocket` in the mailbox for the wake call, released when the handler task ends.

### Bounds, both directions
- **Receive**: when the mailbox holds `max_queued` bytes (default 1 MiB) the engine stops reading the socket (`pause_reading`, called under the
  mailbox lock so it is ordered before the resume); it resumes when the handler has drained to half. TCP then pushes back on the sender.
  Memory per connection is about `max_queued + max_message`.
- **Send**: a send into a full queue (`max_buffered`) is refused by C++; the coroutine waits for `on_drain` and retries. `await send_*` is
  therefore flow control: a handler that sends faster than the peer reads is suspended, never dropped and never unbounded. (A drain flag
  is sticky, so a drain that arrives before the coroutine starts waiting is not lost.)
- A send after closing began waits until the connection has ended so the exception carries the final close code. Because the engine lingers
  up to 2 s for a peer that does not hang up after our close frame, that wait is bounded by it.

### Origin: same origin by default in Python
Browsers do not apply the same-origin policy to WebSockets, and they send cookies, so a page on any site can open a connection to a
logged-in user's server (cross-site WebSocket hijacking). A non-browser client sends no `Origin`. `Server.websocket(origins=None)`
therefore accepts a request without `Origin`, and one whose `Origin` host equals the `Host` header; `origins=[...]` adds exact origins and
`["*"]` allows all. The C++ API stays neutral (it does not check; `on_handshake` can). This is stricter than Starlette's default on purpose.

## Alternatives considered
- Synchronous `on_handshake`, Python called with the GIL on the I/O thread: breaks the "I/O thread never takes the GIL" rule and
  stalls every connection of that thread while Python runs. Rejected.
- Pushing each message into asyncio with `call_soon_threadsafe`: one wake per message and one Python object per message queued without a
  bound. The mailbox gives one wake per burst and a hard bound. Rejected.
- Dropping messages or closing with 1013 when the handler lags: loses data silently for the common case of a briefly busy handler;
  pausing the socket costs nothing and is what TCP is for. Rejected.
- `on_close` at the moment both close frames were exchanged (before the linger): earlier but the final TCP state is then not known. Kept as is.

## Consequences
- One lane (loop thread + inbox) per I/O thread on a free-threaded Python, one with a GIL: connections inherit the lane of the I/O thread
  that accepted them, so all callbacks and the handler of one connection stay on one pair of threads.
- WebSocket handlers are in `_has_async` mode: the lanes start when the server listens.
- Not done: dependencies and OpenAPI for WebSocket routes, `APIRouter.websocket`, permessage-deflate, an Autobahn run, a WebSocket
  benchmark, measurement of memory with many idle connections. None of these is claimed.

## Verification
26 + 1 Python tests (3.14 and 3.14t): echo, ordering of 500 messages, JSON, params, close codes both ways, 1006 on a drop, messages
queued before a close are still read, 403/500/1011, `HTTPException`, subprotocol, Origin, `max_message` 1009, receive bounded (64 MiB offered to a sleeping
handler stops the sender), send waits on a full queue then completes (32 MiB), ping, 60 connections, three I/O threads.
