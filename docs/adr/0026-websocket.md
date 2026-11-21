# ADR-0026: WebSocket: a generic tunnel in the engine, an asynchronous handshake, close 1001 on drain, routers and dependencies

- **Status:** accepted (merged ADR; each part keeps its own status line)
- **Merged on:** 2026-11-21, at the request of the project owner: the decisions of one subject were spread over 4 ADRs. The text of every part is kept, as a section below, in the order it was decided.

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history. Where a part and this summary disagree, this summary wins.

- **The engine knows no WebSocket.** A response with status 101 and a `tunnel` turns the connection into a byte stream; `ws_session` implements RFC 6455 on top of it (frames, fragments, control frames, close handshake, UTF-8 and close-code checks, a bounded send queue, ping).
- **The handshake is answered later, from any thread:** `ws_handler::on_handshake(request, ws_acceptor)` returns at once and the answer is `accept(headers, user)` or `reject(...)`; the first call wins. A dropped acceptor closes the connection without a response; an unanswered one gets a 504 after the async timeout. `on_close` runs exactly once for every accepted upgrade (reported as 1006 when the connection ended before `on_open`). This replaces the synchronous `on_handshake` of the first part.
- **Python** has `@app.websocket(path, ...)` with a Starlette-shaped `websocket` object and bounded per-connection mailboxes, so a slow Python handler pushes back on the peer instead of growing memory.
- **Drain:** `tunnel::on_drain` (default: close) lets `ws_session` send a close frame 1001 ("going away") when the server drains, so a peer can tell a deploy from a network fault.
- **Routers:** `router::route_ws` (C++) and `@router.websocket(path, ...)` (Python) register WebSocket routes on routers with the same prefix rules as HTTP routes, and `Depends` works on WebSocket routes.

## Parts
### WebSocket (RFC 6455) in the C++ core, on the I/O thread

#### Status
Accepted (2026-10-29). Amended by the part "WebSocket in Python, the asynchronous handshake, and bounded mailboxes": the synchronous `on_handshake` described below became an asynchronous one with `ws_acceptor`. The owner asked for WebSocket and delegated the design. This ADR covers the C++ core only; the Python binding
follows in its own ADR addendum (see "Python"). Backpressure is decided here, as `Architecture.md` risk 6 asked (option A, a bounded queue,
from the start).

#### Context
The engine (`async_server`) speaks request/response: one request is parsed, one answer is written, then the next request. A WebSocket
needs the opposite after the handshake: the connection becomes a bidirectional byte stream that either side writes at any time, for hours.
Handlers must be able to send from any thread (Python code, a pool thread, a timer), and a slow client must never make memory grow without bound.

#### Decision

##### 1. The engine gets a generic "tunnel", knows nothing about WebSocket
`wire_response` gets one field, `std::shared_ptr<tunnel> tunnel`. When the status is 101 and a tunnel is set, the engine writes the
head exactly as given (no `Content-Length`, no `Connection` rewriting), and then the connection stops being HTTP:

- bytes read are handed to `tunnel::on_bytes` on the loop thread; bytes pipelined after the upgrade request are handed over first;
- the tunnel writes through `tunnel_io` (loop thread only): `write(string)` queues, the engine writes the queue in order with one
  `async_write` at a time, `queued()` reports the bytes not yet written, `on_written(n)` tells the tunnel how many bytes went out;
- `pause_reading(bool)`, `close()` (now), `close_after_flush()` (write the queue, half-close, drain for the linger time, close),
  `set_idle(seconds)` (call `on_idle` every time that long passes with no bytes received, 0 = never), `set_deadline(ms)` (hard close
  when it passes, 0 cancels);
- a thread-safe `post` function is handed to `on_open`: it runs a function on the loop thread with the `tunnel_io`, and does nothing if the
  connection is gone;
- `on_close` runs exactly once, on the loop thread, however the connection ends (including server stop).
A write that does not complete within the write timeout closes the connection (a client that stopped reading).
The keep-alive, header and body timers do not apply to a tunnel.

##### 2. A WebSocket session is a tunnel (`src/core/internal/websocket.*`, not installed)
- **Handshake** (done by the server, on the I/O thread, like `route_io`): `GET`, HTTP/1.1, `Upgrade: websocket` and `Connection: upgrade` as
  tokens (case-insensitive), `Sec-WebSocket-Key` that is base64 of 16 bytes, `Sec-WebSocket-Version: 13`. Otherwise `400`, or `426` with
  `Sec-WebSocket-Version: 13`. (The HTTP version of the request line is not checked.) The accept key is SHA-1 + base64 through the vendored PSA crypto (SHA-1 is on in its default config; no new dependency).
  The handler's `on_handshake` hook sees the request and may refuse (its response is sent) or add headers such as `Sec-WebSocket-Protocol`
  (which must be one the client offered, else 500). `Origin` is not checked by default: a browser app must check it in `on_handshake`
  (cross-site WebSocket hijacking). The 101 is sent directly: the `after` hook does not run for it; the error handler and hooks do run for refusals.
- **Frames:** text, binary, continuation, ping, pong, close. Server frames are never masked, client frames must be (else 1002). RSV bits
  must be 0 (no extensions, so `permessage-deflate` is never negotiated; the client's extension offer is ignored, which RFC 6455 allows).
  Reserved opcodes, a fragmented or over-125-byte control frame, a continuation without a start, a new data frame inside a fragmented
  message, a 64-bit length with the top bit set: 1002. Text that is not UTF-8 (checked when the message is complete): 1007.
  A close payload of one byte, an invalid code (allowed: 1000-1003, 1007-1014, 3000-4999), or a reason that is not UTF-8: 1002 / 1007.
- **Limits:** `max_message` (default 1 MiB, per route) is checked against the frame header before one byte of payload is buffered: 1009.
- **Ping/pong:** a ping is answered with a pong of the same payload (unless closing). With `ping_interval` seconds (default 0 = off), a
  ping is sent after that long without bytes; if still nothing arrives in the next interval the connection is closed (1001 sent, then dropped).
- **Close handshake:** a close from the peer is echoed (its code, or 1002 when invalid) and the connection ends after the flush. `close(code, reason)`
  from our side sends the frame, then waits up to 5 s for the peer's close frame (`set_deadline`), then drops. `on_close` gets the code the peer
  sent, or 1006 if the connection ended without a close frame.

##### 3. Public C++ API (`httpp/websocket.hpp`, `server::route_ws`)
```cpp
class websocket {            // a cheap copyable handle; every member is thread-safe
public:
    enum class status { ok, closed, full };
    status send_text(std::string_view);
    status send_binary(std::string_view);
    status ping(std::string_view = {});            // payload at most 125 bytes
    void close(std::uint16_t code = 1000, std::string_view reason = {});
    void pause_reading(); void resume_reading();   // for a handler that hands messages to another thread
    bool is_open() const;
    std::size_t buffered_amount() const;           // bytes queued, not yet written
    const request& handshake() const;              // the upgrade request
};
struct ws_handler {          // all callbacks run on the I/O thread: they must not block
    std::function<bool(const request&, response&)> on_handshake;                 // optional; false = refuse with `response`
    std::function<void(const websocket&)> on_open;                               // optional
    std::function<void(const websocket&, std::string_view, bool binary)> on_message; // the view is valid during the call only
    std::function<void(const websocket&)> on_drain;                              // optional: the queue fell to half after a `full`
    std::function<void(const websocket&, std::uint16_t, std::string_view)> on_close; // optional, exactly once
    std::size_t max_message = 1 << 20;
    std::size_t max_buffered = 1 << 20;
    int ping_interval = 0;
};
void server::route_ws(const std::string& path, ws_handler h);                    // GET only; path params work as for route
```
A send made on the I/O thread of the connection writes straight into the queue; from any other thread it is posted to the loop (the order
of one thread's sends is kept).

##### 4. Backpressure (bounded queue, `Architecture.md` risk 6, option A)
Outgoing: `send_*` counts the bytes it queued in an atomic. If the queue already holds `max_buffered` bytes the message is **not** queued and
`status::full` is returned (never a silent drop); `on_drain` tells the producer when it may continue (when the queue has fallen to half of `max_buffered`, so a producer does not stop and start on every write). The queue is bounded by
`max_buffered` plus one message, and by the write timeout. Incoming: handlers run on the loop thread and return at once, so reading is paced by
them; a handler that moves messages to another thread calls `pause_reading()` when its own queue is long and `resume_reading()` when it drains
(TCP then pushes back on the peer).

##### 5. Python (next step, not in this change)
The Python binding follows the lanes design (ADR-0024): the handler of a connection runs on the asyncio loop of the lane of its I/O thread, so a
connection never changes thread and `send` from its own loop is the direct path. A free-threaded Python gets one loop per I/O thread as for `async def`.

#### Rejected alternatives
- **A WebSocket library (libwebsockets, uWebSockets, Beast):** a new dependency and its own event loop; the whole point of the engine is to own the loop.
- **WebSocket logic inside `async_server.cpp`:** the engine is already 750 lines; a tunnel interface keeps it generic (it is also what any other
  upgrade, such as `h2c` or a raw TCP tunnel, would need) and lets the codec be tested without sockets.
- **Handlers on the worker pool:** one pool thread per open connection is exactly the cost the engine exists to avoid, and a worker hop per message
  adds latency and cross-thread frees (see ADR-0024 addendum 6).
- **An unbounded queue, or dropping the oldest message:** memory DoS, or silent data loss that the producer cannot see.
- **Validating UTF-8 per fragment:** fails fast on the first bad fragment, but costs state across fragments; checked at the end of the message for now,
  which is correct and only later in the invalid case (the `max_message` limit still bounds it).

#### Consequences
- `route_ws` handlers share `route_io`'s rule: never block. The I/O thread count (`set_io_threads`) applies: a connection lives on one thread.
- No `permessage-deflate`, no `wss://` of its own: the server has no TLS yet (`Architecture.md`), so `wss` needs a TLS-terminating proxy.
- The server stopping closes tunnels without a close frame; `on_close` reports 1006.
- The unit tests of the frame rules run without sockets (`test_websocket_session.cpp`, 41 tests); the engine and server are tested with a bare
  socket client (`test_server_websocket.cpp`, 28 tests: handshake errors, echo of 1.5 MB in fragments, close handshake, 1002/1009, ping, a thread
  sending and closing, three I/O threads, the full queue and `on_drain`, the write timeout against a peer that never reads, server stop).
- **Not verified yet:** the Autobahn test suite (the unit tests cover each rule above but are not a conformance run), behaviour on Windows and macOS,
  and memory under many idle connections.

#### Related
ADR-0023 and 0024; `Architecture.md` risk 6; `checklist.md` phase 5.

### WebSocket in Python, the asynchronous handshake, and bounded mailboxes

#### Status
Accepted (2026-10-30). The owner delegated every design choice ("implement the most principled and optimal way, the ADRs may be wrong").
This ADR amends the part "WebSocket (RFC 6455) in the C++ core, on the I/O thread": the handshake hook described there (a synchronous `on_handshake(request, response&)`)
is replaced by the asynchronous `ws_acceptor` below, and `Origin` handling changes in the Python layer.

#### Context
the part "WebSocket (RFC 6455) in the C++ core, on the I/O thread" delivered WebSocket in the C++ core. Python needs the same thing with `async def` handlers, and three facts shaped the design:

1. A Python handler cannot answer the upgrade inside the I/O thread callback: it lives on an asyncio loop thread and needs the GIL. A
   synchronous hook would force the I/O thread to take the GIL (ADR-0024 forbids that) or to block.
2. Per-connection state must be reachable from the callbacks (all on the I/O thread) without a global map.
3. A slow Python handler must not make memory grow: receiving and sending both need a bound that pushes back on the peer.

#### Decision

##### C++: the handshake is answered later, from any thread
`ws_handler::on_handshake(std::shared_ptr<const request>, ws_acceptor)` runs on the I/O thread for an upgrade that passed the protocol
checks and returns at once. The answer comes from any thread: `acceptor.accept(headers, user)` or `acceptor.reject(...)`. The first call wins.
A dropped acceptor closes the connection without a response; an unanswered one gets a 504 after `set_async_timeout`, like `route_async`.
The engine does not read from a connection while its upgrade waits for an answer.
`accept` takes an opaque `std::shared_ptr<void> user`, returned by `websocket::user()` in every callback, so per-connection state needs no map.
`on_close` runs **exactly once for every accepted upgrade**, also when the connection ended before `on_open` (reported as 1006), because
the session's destructor reports it. Without a hook every valid upgrade is accepted immediately (unchanged).

##### Python: one task per connection, a mailbox per connection
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

##### Bounds, both directions
- **Receive**: when the mailbox holds `max_queued` bytes (default 1 MiB) the engine stops reading the socket (`pause_reading`, called under the
  mailbox lock so it is ordered before the resume); it resumes when the handler has drained to half. TCP then pushes back on the sender.
  Memory per connection is about `max_queued + max_message`.
- **Send**: a send into a full queue (`max_buffered`) is refused by C++; the coroutine waits for `on_drain` and retries. `await send_*` is
  therefore flow control: a handler that sends faster than the peer reads is suspended, never dropped and never unbounded. (A drain flag
  is sticky, so a drain that arrives before the coroutine starts waiting is not lost.)
- A send after closing began waits until the connection has ended so the exception carries the final close code. Because the engine lingers
  up to 2 s for a peer that does not hang up after our close frame, that wait is bounded by it.

##### Origin: same origin by default in Python
Browsers do not apply the same-origin policy to WebSockets, and they send cookies, so a page on any site can open a connection to a
logged-in user's server (cross-site WebSocket hijacking). A non-browser client sends no `Origin`. `Server.websocket(origins=None)`
therefore accepts a request without `Origin`, and one whose `Origin` host equals the `Host` header; `origins=[...]` adds exact origins and
`["*"]` allows all. The C++ API stays neutral (it does not check; `on_handshake` can). This is stricter than Starlette's default on purpose.

#### Alternatives considered
- Synchronous `on_handshake`, Python called with the GIL on the I/O thread: breaks the "I/O thread never takes the GIL" rule and
  stalls every connection of that thread while Python runs. Rejected.
- Pushing each message into asyncio with `call_soon_threadsafe`: one wake per message and one Python object per message queued without a
  bound. The mailbox gives one wake per burst and a hard bound. Rejected.
- Dropping messages or closing with 1013 when the handler lags: loses data silently for the common case of a briefly busy handler;
  pausing the socket costs nothing and is what TCP is for. Rejected.
- `on_close` at the moment both close frames were exchanged (before the linger): earlier but the final TCP state is then not known. Kept as is.

#### Consequences
- One lane (loop thread + inbox) per I/O thread on a free-threaded Python, one with a GIL: connections inherit the lane of the I/O thread
  that accepted them, so all callbacks and the handler of one connection stay on one pair of threads.
- WebSocket handlers are in `_has_async` mode: the lanes start when the server listens.
- Not done: dependencies and OpenAPI for WebSocket routes, `APIRouter.websocket`, permessage-deflate, an Autobahn run, a WebSocket
  benchmark, measurement of memory with many idle connections. None of these is claimed.

#### Verification
26 + 1 Python tests (3.14 and 3.14t): echo, ordering of 500 messages, JSON, params, close codes both ways, 1006 on a drop, messages
queued before a close are still read, 403/500/1011, `HTTPException`, subprotocol, Origin, `max_message` 1009, receive bounded (64 MiB offered to a sleeping
handler stops the sender), send waits on a full queue then completes (32 MiB), ping, 60 connections, three I/O threads.

### WebSocket connections get a close 1001 frame when the server drains

- **Status:** accepted
- **Source:** `tunnel::on_drain` in `src/core/internal/async_server.hpp`, `impl::begin_drain` in `src/core/async_server.cpp`, `ws_session::on_drain` in `src/core/websocket.cpp`,
  `tests/cpp/test_websocket_session.cpp`, `tests/cpp/test_server_shutdown.cpp`, `tests/python/test_shutdown.py`

#### Context

ADR-0038 made `shutdown(grace)` close every tunnel (WebSocket) connection abruptly at the start of the drain, and listed a close frame 1001 ("going away") as the right
behaviour that needed a hook into the session layer: the tunnel interface only had `close()`. A peer that sees the TCP connection die without a close frame reports 1006
(abnormal closure) and cannot tell a deploy from a network fault, so a browser client may not reconnect with the right delay, and a handler never sees a clean close.

#### Decision

1. `tunnel` gets a virtual `on_drain(tunnel_io&)`. The default calls `io.close()`, which is the old behaviour, so any other tunnel keeps working unchanged.
2. `async_server::impl::begin_drain` calls `on_drain` on every tunnel connection instead of closing it. Connections that wait for the first byte of a request are
   still closed at once. A request in progress is still left alone.
3. `ws_session::on_drain` does what `websocket::close(1001)` does: it sends a close frame with code 1001 and an empty reason, stops accepting sends, and waits for the peer's
   close frame (the existing 5 second close deadline applies). When the peer answers, the connection ends through the normal close path and the handler's `on_close` sees
   the peer's code. If a close was already started, nothing changes.
4. The grace period of the drain is the limit: the engine cuts whatever is still open when it ends, as it does for a request in progress. `listen()` therefore
   returns when the last WebSocket has answered, or when the grace period is over.
5. A WebSocket whose upgrade answer reaches the loop after the drain began (the 101 is produced on a worker thread and posted) was not a tunnel yet when `begin_drain` looked at
   the connections. `tunnel_begin` therefore calls `on_drain` itself when the engine is already draining, right after `on_open`.
6. No new public API: C++ `ws_handler` and the Python `websocket` object are unchanged (the Python handler's `receive()` raises `WebSocketDisconnect` with code 1001).

Note on how it ends: after the peer's close frame the engine half-closes the TCP connection and waits for the peer to close its end, for at most 2 seconds (the engine's linger time, the same
as for a refused request). A client that answers the close frame but never closes its socket therefore holds `listen()` for up to 2 seconds more. A browser closes its end at once.

#### Rejected alternatives

- A public `on_shutdown` callback on `ws_handler`, so an app can send its own goodbye first: it would add API to both languages for something the close frame already says;
  a handler that wants to react can use `on_close` with code 1001.
- Closing with 1012 ("service restart"): 1012 is what a restart means to clients, but a drain also happens for a plain stop; 1001 is the neutral, widely understood code.
  The owner can change the constant in one place.
- Waiting for the peer without a limit: one silent client would hold the shutdown for the whole grace period for nothing; the existing 5 second close deadline is kept.

#### Consequences

- A shutdown with many open WebSockets writes one small frame per connection and then waits for their answers; with `--shutdown-timeout` shorter than the close deadline the
  grace period ends first and the rest are cut.
- `ponytail:` the close reason is empty. Upgrade path: a `ws_limits` field or a `server` option for the reason text.
- Verified: 2 new session tests with a fake tunnel_io, 3 new server tests (the 1001 frame and a clean end when the peer answers, a silent peer cut after the grace period,
  the handler sees code 1001), 2 new Python tests.
- Not verified: Windows and macOS, a browser client, WebSocket over TLS (`wss`) during a drain, many connections at once.

### WebSocket routes on routers, and dependencies on WebSocket routes

- **Status:** accepted
- **Source:** `router::route_ws` / `ws_routes` in `include/httpp/router.hpp` and `src/core/router.cpp`, `APIRouter.websocket` in `src/bindings/python/httpp/_router.py`,
  `_adapt_ws` and `Server.websocket` in `src/bindings/python/httpp/server.py`, `Ctx` and `Plan.wants_websocket` in `src/bindings/python/httpp/_params.py`,
  `tests/cpp/test_router.cpp`, `tests/python/test_websocket_router.py`

#### Context

A WebSocket route could only be registered on a `Server`, so an app split into routers had to keep its WebSocket endpoints outside them, and a WebSocket handler could not use
`Depends` (authentication, a database session, a shared limit), which FastAPI users expect.

#### Decision

1. **Routers.** `router::route_ws(path, ws_handler)` (C++) and `@router.websocket(path, **options)` (Python) record the route. `include` joins prefixes exactly as for HTTP routes (outer prefix,
   router prefix, path), and `server::include` registers each one with `route_ws` / `Server.websocket`. Python options are checked when the route is declared, with the same rules as
   `Server.websocket` (`_check_ws_options`). Tags do not apply (WebSocket routes are not in OpenAPI).
2. **Dependencies (Python only: C++ has no dependency system).** Dependencies are solved before the handler in a per-connection `AsyncExitStack`, so `yield` dependencies are closed after the
   handler ends, also after an exception, last opened first. A validation error in a dependency refuses the upgrade with 422 (the same body as path errors); an `HTTPException` refuses it with
   its status. A dependency may take `websocket` (by name or annotation) to accept or close the connection itself. Using such a dependency on an HTTP route is a `TypeError` at registration.
3. Still refused on WebSocket routes: `response` and background tasks (nothing to attach them to). WebSocket routes stay out of the OpenAPI document, which cannot describe them in 3.1.

#### Rejected alternatives

- Solving dependencies in the C++ handshake callback: the work is Python and async, and the Python task already answers the upgrade (accept / reject).
- A separate `WebSocketDepends`: one marker is enough, the connection is injected by name like `request`.
