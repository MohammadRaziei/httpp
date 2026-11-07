# ADR-0042: `httpp.testclient.TestClient` serves the app on a loopback port

- **Status:** accepted
- **Source:** `src/bindings/python/httpp/testclient.py`, `Server._unhandled` and `Server._exception_sink` in `server.py`, `tests/python/test_testclient.py`

## Context

Users need to test their own apps: `client.get("/items/3")` in a pytest function, with cookies, redirects, startup hooks and errors, without starting a server by hand
(the pattern in our own tests is `bind_to_any_port` plus a thread plus `sleep`). `docs/agent.md` already asked for a Starlette-shaped `TestClient` and said: "default
mode: a real server on loopback (it also exercises our own engine; better than Starlette's in-memory mode); an in-memory mode later".
Routing, middleware, validation, gzip and the exception paths are implemented in the C++ core and in `server.py`; an in-memory mode would need a second copy of all of that
in Python, which would drift.

## Decision

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
   first call `Server._unhandled(exc)`, which appends to `Server._exception_sink` when a test client has set it. The client re-raises the first one after the response
   (the server still answers 500, so the connection state is normal). Without a test client the sink is `None` and nothing changes.

## Rejected alternatives

- An in-memory mode now: a second router and middleware chain to keep in step with C++. Left for later, only if a need shows that a socket cannot meet (sandboxes without loopback).
- Subclassing `httpp.Response`: it is a compiled class built from a C++ response; a small Python class holding it is simpler and gives the httpx-like names.
- A fixed port: collisions between tests and between parallel runs.

## Consequences

- A test needs a loopback socket; each request is a real HTTP round trip (about a millisecond).
- The scheme of `base_url` only shows in `response.url`; `request.secure` is always false.
- `Server._exception_sink` is a private hook that costs one attribute read on the failure path only.
- Not done: `websocket_connect` (httpp has no WebSocket client), multipart `files=`, in-memory mode, `client=("testclient", 50000)`, cookie Domain/Path/Secure matching.
- Verified on Linux x86_64 (2026-11-07): `tests/python/test_testclient.py`, 22 tests, and the whole Python suite on 3.13 (805 passed, 4 skipped) and 3.14t (804 passed, 5 skipped).
  Not verified: Windows, macOS, a sandbox without loopback, many clients at once on one app.

## Related

ADR-0038 (graceful stop used by `close()`), `docs/agent.md` (Starlette test suite plan), ADR-0041.
