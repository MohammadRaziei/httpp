# ADR-0042: `httpp.testclient.TestClient`: a real server on a loopback port, with `websocket_connect` and multipart `files=`

- **Status:** accepted (merged ADR; each part keeps its own status line)
- **Merged on:** 2026-11-21, at the request of the project owner: the decisions of one subject were spread over 2 ADRs. The text of every part is kept, as a section below, in the order it was decided.

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history. Where a part and this summary disagree, this summary wins.

- **`TestClient(app)` starts the app on a loopback port** (`bind_to_any_port` plus `listen_after_bind` on a daemon thread) and sends real requests through `httpp.request`, so tests exercise our own engine; there is no in-memory mode. `with TestClient(app)` starts and stops it; without `with` the first request starts it and `close()` stops it.
- **The interface follows Starlette's:** `get/head/options/post/put/patch/delete/request`, `params`, `headers`, `json`, `data`, `content`, `cookies`, `auth`, `follow_redirects`, `timeout`, `base_url`, `raise_server_exceptions`; the response has `status_code`, `headers`, `content`, `text`, `json()`, `cookies`, `url`, `history`, `reason_phrase`, `is_success`, `is_redirect`, `raise_for_status()`.
- **`files=`** sends multipart (a dict or a list of `(field, file)` pairs; a `data` dict next to it becomes plain fields).
- **`websocket_connect(url, subprotocols, headers, cookies, timeout)`** opens a socket to the app's loopback port and does the RFC 6455 handshake itself; any answer but 101 raises `httpp.WebSocketDisconnect`.

## Parts
### `httpp.testclient.TestClient` serves the app on a loopback port

- **Status:** accepted
- **Source:** `src/bindings/python/httpp/testclient.py`, `_RequestState.unhandled` and `Server._exception_sink` (a property on the `_RequestState` of the server) in `server.py`, `tests/python/test_testclient.py`

#### Context

Users need to test their own apps: `client.get("/items/3")` in a pytest function, with cookies, redirects, startup hooks and errors, without starting a server by hand
(the pattern in our own tests is `bind_to_any_port` plus a thread plus `sleep`). `docs/agent.md` already asked for a Starlette-shaped `TestClient` and said: "default
mode: a real server on loopback (it also exercises our own engine; better than Starlette's in-memory mode); an in-memory mode later".
Routing, middleware, validation, gzip and the exception paths are implemented in the C++ core and in `server.py`; an in-memory mode would need a second copy of all of that
in Python, which would drift.

#### Decision

1. `TestClient(app)` starts the app with `bind_to_any_port("127.0.0.1")` and `listen_after_bind()` on a daemon thread, waits until a connection is accepted
   (the startup hooks have run by then) and sends requests through `httpp.request`. `with TestClient(app)` starts and stops it; without `with` the first request starts it and
   `close()` stops it; a closed client can start again on a new port.
2. The interface follows Starlette's: `get/head/options/post/put/patch/delete/request`, `params`, `headers`, `json`, `data` (dict = form, bytes/str raw), `content`,
   `cookies`, `auth`, `follow_redirects` (default True), `timeout`, `base_url` (default `http://testserver`, sent as the `Host` header), `raise_server_exceptions`
   (default True). The response has `status_code`, `headers` (case-insensitive), `content`, `text`, `json()`, `cookies`, `url`, `history`, `reason_phrase`,
   `is_success`, `is_redirect`, `raise_for_status()`.
3. Cookies the server sets are kept by name in `client.cookies` and sent back; `Max-Age=0` or a past `Expires` deletes. Redirects are followed by the client itself (up to 20,
   `TooManyRedirects` after that; 303, and 301/302 after POST, become GET) so cookies and `history` work across hops.
4. `raise_server_exceptions`: when a handler raises and no exception handler answers, the three places that re-raise for the core (sync, sync with dependencies, async)
   first call `_RequestState.unhandled(exc)`, which appends to the sink (`Server._exception_sink`) when a test client has set it. The client re-raises the first one after the response
   (the server still answers 500, so the connection state is normal). Without a test client the sink is `None` and nothing changes.

#### Rejected alternatives

- An in-memory mode now: a second router and middleware chain to keep in step with C++. Left for later, only if a need shows that a socket cannot meet (sandboxes without loopback).
- Subclassing `httpp.Response`: it is a compiled class built from a C++ response; a small Python class holding it is simpler and gives the httpx-like names.
- A fixed port: collisions between tests and between parallel runs.

#### Consequences

- A test needs a loopback socket; each request is a real HTTP round trip (about a millisecond).
- The scheme of `base_url` only shows in `response.url`; `request.secure` is always false.
- `Server._exception_sink` is a private hook that costs one attribute read on the failure path only.
- Not done: in-memory mode (`websocket_connect` and multipart `files=` came with the part "`TestClient.websocket_connect` and multipart `files=`"), `client=("testclient", 50000)`, cookie Domain/Path/Secure matching.
- Verified on Linux x86_64 (2026-11-07): `tests/python/test_testclient.py`, 22 tests, and the whole Python suite on 3.13 (805 passed, 4 skipped) and 3.14t (804 passed, 5 skipped).
  Not verified: Windows, macOS, a sandbox without loopback, many clients at once on one app.

#### Related

ADR-0038 (graceful stop used by `close()`), `docs/agent.md` (Starlette test suite plan), ADR-0041.

### `TestClient.websocket_connect` and multipart `files=`

- **Status:** accepted
- **Source:** `WebSocketTestSession`, `_multipart` and `TestClient.websocket_connect` in `src/bindings/python/httpp/testclient.py`, `tests/python/test_testclient.py`
- **Extends:** the part "`httpp.testclient.TestClient` serves the app on a loopback port"

#### Context

the part "`httpp.testclient.TestClient` serves the app on a loopback port" left out two things that people coming from Starlette use in tests: `client.websocket_connect(...)` and `files=` on a request. httpp had no WebSocket client (only the
server side, and the bare socket helper in our own tests), and the request helper could send a urlencoded form but not multipart.

#### Decision

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

#### Rejected alternatives

- A public `httpp.websocket_connect` client: a real feature with its own design (TLS, redirects, extensions, a C++ twin by the dual-API rule). The test session is only for tests.
- Reusing the helper in `tests/python/test_websocket.py`: it lives in the test tree, and shipping it means owning its API anyway.
- `files=` with a bare `bytes` value as a plain field (what httpx does): the server only reports a part as a file when it has a filename, so a test that passes bytes would not see an upload.
- Sending `Content-Type` from `files` only when given: always sending it makes `UploadFile.content_type` predictable in tests.

#### Consequences

- `ponytail:` the session reads with blocking calls and the socket timeout of the client (30 seconds by default), so a test that waits for a message that never comes
  fails with `TimeoutError` after that long. Upgrade path: a `timeout` argument on `receive_*`.
- `ponytail:` no `permessage-deflate` (the handshake offers no extensions), no `wss`, and the connection is plain loopback like the requests.
- Not done: an in-memory mode (the part "`httpp.testclient.TestClient` serves the app on a loopback port" still holds), `receive` with a timeout of its own.
- Verified (Linux, Python 3.13): 11 new tests: files with fields, bytes, pairs, file objects, a list, a quoted and non-ASCII filename, the three `ValueError` cases; echo of text, binary and
  JSON in both modes, use without `with` and before any request, the server's close code and reason, a refused handshake and an unknown route, cookies and headers and subprotocols in the handshake, the
  client's close with a code (twice), messages sent back to back.
- Not verified: Windows and macOS, a server that sends a fragmented message or a ping (no httpp API sends either, so only the code path is written), large messages (over 64 KiB).
