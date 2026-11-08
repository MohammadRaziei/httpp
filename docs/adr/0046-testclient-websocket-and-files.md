# ADR-0046: `TestClient.websocket_connect` and multipart `files=`

- **Status:** accepted
- **Source:** `WebSocketTestSession`, `_multipart` and `TestClient.websocket_connect` in `src/bindings/python/httpp/testclient.py`, `tests/python/test_testclient.py`
- **Extends:** ADR-0042

## Context

ADR-0042 left out two things that people coming from Starlette use in tests: `client.websocket_connect(...)` and `files=` on a request. httpp had no WebSocket client (only the
server side, and the bare socket helper in our own tests), and the request helper could send a urlencoded form but not multipart.

## Decision

1. `files=` is a dict or a list of `(field, file)` pairs. A file is bytes or str (a file named after its field), a file object (its `name` gives the filename), or
   `(filename, content)` or `(filename, content, content_type)`. The content type is guessed from the filename and falls back to `application/octet-stream`. A `data` dict next to
   `files` becomes plain fields of the same multipart body (a list value repeats the field). `files` with `json`, `content` or a raw `data` body is a `ValueError`.
   A quote in a name or filename is written as `\"`, which is what httpp's parser undoes; a line break becomes a space.
2. `websocket_connect(url, subprotocols=None, headers=None, cookies=None, timeout=None)` starts the app like a request does, opens a socket to its loopback port, and does the
   RFC 6455 handshake itself, with the client's `headers`, its cookies, and `Host` from `base_url`. The `Sec-WebSocket-Accept` answer is checked. Any answer but 101 raises
   `httpp.WebSocketDisconnect` (code 1006, the reason names the HTTP status).
3. The result, `WebSocketTestSession`, is a context manager with `send_text`, `send_bytes`, `send_json(data, mode="text"|"binary")`, `receive()` (a Starlette-shaped dict, also for
   the close), `receive_text`, `receive_bytes`, `receive_json`, `close(code=1000, reason="")` and `accepted_subprotocol`. The `receive_*` methods raise `WebSocketDisconnect`
   with the server's code and reason when it closed. Fragmented messages are joined, a ping is answered with a pong and not shown, and the server's close frame is answered
   with ours. `close()` sends a close frame, waits up to 2 seconds for the server's, and drops the socket; calling it again does nothing.
4. The client is about 150 lines of standard-library code with no dependency. It is deliberately separate from the server's engine: a test of the engine would otherwise use the code it tests.

## Rejected alternatives

- A public `httpp.websocket_connect` client: a real feature with its own design (TLS, redirects, extensions, a C++ twin by the dual-API rule). The test session is only for tests.
- Reusing the helper in `tests/python/test_websocket.py`: it lives in the test tree, and shipping it means owning its API anyway.
- `files=` with a bare `bytes` value as a plain field (what httpx does): the server only reports a part as a file when it has a filename, so a test that passes bytes would not see an upload.
- Sending `Content-Type` from `files` only when given: always sending it makes `UploadFile.content_type` predictable in tests.

## Consequences

- `ponytail:` the session reads with blocking calls and the socket timeout of the client (30 seconds by default), so a test that waits for a message that never comes
  fails with `TimeoutError` after that long. Upgrade path: a `timeout` argument on `receive_*`.
- `ponytail:` no `permessage-deflate` (the handshake offers no extensions), no `wss`, and the connection is plain loopback like the requests.
- Not done: an in-memory mode (ADR-0042 still holds), `receive` with a timeout of its own.
- Verified (Linux, Python 3.13): 11 new tests: files with fields, bytes, pairs, file objects, a list, a quoted and non-ASCII filename, the three `ValueError` cases; echo of text, binary and
  JSON in both modes, use without `with` and before any request, the server's close code and reason, a refused handshake and an unknown route, cookies and headers and subprotocols in the handshake, the
  client's close with a code (twice), messages sent back to back.
- Not verified: Windows and macOS, a server that sends a fragmented message or a ping (no httpp API sends either, so only the code path is written), large messages (over 64 KiB).
