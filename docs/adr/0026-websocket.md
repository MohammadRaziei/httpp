# ADR-0026: WebSocket (RFC 6455) in the C++ core, on the I/O thread

## Status
Accepted (2026-10-29). The owner asked for WebSocket and delegated the design. This ADR covers the C++ core only; the Python binding
follows in its own ADR addendum (see "Python"). Backpressure is decided here, as `Architecture.md` risk 6 asked (option A, a bounded queue,
from the start).

## Context
The engine (`async_server`) speaks request/response: one request is parsed, one answer is written, then the next request. A WebSocket
needs the opposite after the handshake: the connection becomes a bidirectional byte stream that either side writes at any time, for hours.
Handlers must be able to send from any thread (Python code, a pool thread, a timer), and a slow client must never make memory grow without bound.

## Decision

### 1. The engine gets a generic "tunnel", knows nothing about WebSocket
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

### 2. A WebSocket session is a tunnel (`src/core/internal/websocket.*`, not installed)
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

### 3. Public C++ API (`httpp/websocket.hpp`, `server::route_ws`)
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

### 4. Backpressure (bounded queue, `Architecture.md` risk 6, option A)
Outgoing: `send_*` counts the bytes it queued in an atomic. If the queue already holds `max_buffered` bytes the message is **not** queued and
`status::full` is returned (never a silent drop); `on_drain` tells the producer when it may continue (when the queue has fallen to half of `max_buffered`, so a producer does not stop and start on every write). The queue is bounded by
`max_buffered` plus one message, and by the write timeout. Incoming: handlers run on the loop thread and return at once, so reading is paced by
them; a handler that moves messages to another thread calls `pause_reading()` when its own queue is long and `resume_reading()` when it drains
(TCP then pushes back on the peer).

### 5. Python (next step, not in this change)
The Python binding follows the lanes design (ADR-0024): the handler of a connection runs on the asyncio loop of the lane of its I/O thread, so a
connection never changes thread and `send` from its own loop is the direct path. A free-threaded Python gets one loop per I/O thread as for `async def`.

## Rejected alternatives
- **A WebSocket library (libwebsockets, uWebSockets, Beast):** a new dependency and its own event loop; the whole point of the engine is to own the loop.
- **WebSocket logic inside `async_server.cpp`:** the engine is already 750 lines; a tunnel interface keeps it generic (it is also what any other
  upgrade, such as `h2c` or a raw TCP tunnel, would need) and lets the codec be tested without sockets.
- **Handlers on the worker pool:** one pool thread per open connection is exactly the cost the engine exists to avoid, and a worker hop per message
  adds latency and cross-thread frees (see ADR-0025 addendum 6).
- **An unbounded queue, or dropping the oldest message:** memory DoS, or silent data loss that the producer cannot see.
- **Validating UTF-8 per fragment:** fails fast on the first bad fragment, but costs state across fragments; checked at the end of the message for now,
  which is correct and only later in the invalid case (the `max_message` limit still bounds it).

## Consequences
- `route_ws` handlers share `route_io`'s rule: never block. The I/O thread count (`set_io_threads`) applies: a connection lives on one thread.
- No `permessage-deflate`, no `wss://` of its own: the server has no TLS yet (`Architecture.md`), so `wss` needs a TLS-terminating proxy.
- The server stopping closes tunnels without a close frame; `on_close` reports 1006.
- The unit tests of the frame rules run without sockets (`test_websocket_session.cpp`, 41 tests); the engine and server are tested with a bare
  socket client (`test_server_websocket.cpp`, 28 tests: handshake errors, echo of 1.5 MB in fragments, close handshake, 1002/1009, ping, a thread
  sending and closing, three I/O threads, the full queue and `on_drain`, the write timeout against a peer that never reads, server stop).
- **Not verified yet:** the Autobahn test suite (the unit tests cover each rule above but are not a conformance run), behaviour on Windows and macOS,
  and memory under many idle connections.

## Related
ADR-0023, ADR-0024, ADR-0025; `Architecture.md` risk 6; `checklist.md` phase 5.
