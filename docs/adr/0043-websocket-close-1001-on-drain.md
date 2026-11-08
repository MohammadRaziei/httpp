# ADR-0043: WebSocket connections get a close 1001 frame when the server drains

- **Status:** accepted
- **Source:** `tunnel::on_drain` in `src/core/internal/async_server.hpp`, `impl::begin_drain` in `src/core/async_server.cpp`, `ws_session::on_drain` in `src/core/websocket.cpp`,
  `tests/cpp/test_websocket_session.cpp`, `tests/cpp/test_server_shutdown.cpp`, `tests/python/test_shutdown.py`

## Context

ADR-0038 made `shutdown(grace)` close every tunnel (WebSocket) connection abruptly at the start of the drain, and listed a close frame 1001 ("going away") as the right
behaviour that needed a hook into the session layer: the tunnel interface only had `close()`. A peer that sees the TCP connection die without a close frame reports 1006
(abnormal closure) and cannot tell a deploy from a network fault, so a browser client may not reconnect with the right delay, and a handler never sees a clean close.

## Decision

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

## Rejected alternatives

- A public `on_shutdown` callback on `ws_handler`, so an app can send its own goodbye first: it would add API to both languages for something the close frame already says;
  a handler that wants to react can use `on_close` with code 1001.
- Closing with 1012 ("service restart"): 1012 is what a restart means to clients, but a drain also happens for a plain stop; 1001 is the neutral, widely understood code.
  The owner can change the constant in one place.
- Waiting for the peer without a limit: one silent client would hold the shutdown for the whole grace period for nothing; the existing 5 second close deadline is kept.

## Consequences

- A shutdown with many open WebSockets writes one small frame per connection and then waits for their answers; with `--shutdown-timeout` shorter than the close deadline the
  grace period ends first and the rest are cut.
- `ponytail:` the close reason is empty. Upgrade path: a `ws_limits` field or a `server` option for the reason text.
- Verified: 2 new session tests with a fake tunnel_io, 3 new server tests (the 1001 frame and a clean end when the peer answers, a silent peer cut after the grace period,
  the handler sees code 1001), 2 new Python tests.
- Not verified: Windows and macOS, a browser client, WebSocket over TLS (`wss`) during a drain, many connections at once.
