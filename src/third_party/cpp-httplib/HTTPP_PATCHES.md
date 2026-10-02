# httpp patches to cpp-httplib

`httplib.h` here is cpp-httplib **0.54.1** plus the two patches below. Every changed line in the header
is marked with `httpp patch`. `HTTPP_PATCHES.patch` is the same change as a diff against pristine 0.54.1.

## 1. Deferred ("detached") responses

**Why.** cpp-httplib answers a request when the route handler returns, so a handler that has to wait
(for a database, another service, an asyncio coroutine) blocks one worker thread for the whole wait. That
caps concurrency at the thread-pool size. This patch lets a handler hand the response over and return,
which is what makes real `async def` handlers possible in the Python binding.

**API** (namespace `httplib`):

```cpp
svr.Get("/x", [](const httplib::Request &req, httplib::Response &res) {
  auto detached = res.detach();           // keep this handle
  // ... return now; the worker thread is free ...
  // later, from ANY thread:
  httplib::Response r;
  r.set_content("done", "text/plain");
  detached->complete(std::move(r));       // writes the response, then closes the connection
});
```

**Rules**

- `complete()` is thread-safe; only the first call has any effect; it may even run before the handler
  has returned.
- The connection is closed after a detached response, unless the server is event-driven (patch 2) and
  the client allows keep-alive; then it goes back to the event loop for its next request.
- Plain-HTTP `Server` only. On `SSLServer` a detached response is answered with a 500 instead of hanging.
- If the `Server` is destroyed while responses are pending, their connections are closed and a later
  `complete()` does nothing. If the last handle is dropped without `complete()`, the connection is
  closed without a response.
- A handler that throws after `detach()` is treated as an ordinary failed handler (the detach is
  discarded and the usual 500 path runs).

**What changed**

| Where | Change |
|---|---|
| `struct Response` | new `detach()` and a private `detached_` member |
| new `class DetachedResponse` | the handle: mutex-protected state, the socket, a copy of the `Request` |
| `Server::process_request` | new trailing `bool *detached_out` parameter; after routing, a detached response is *attached* (socket and request are taken over) instead of written |
| `Server::process_and_close_socket` | does not close the socket when it was taken over |
| `Server` | `attach_detached()`, `write_detached()`, `abandon_detached()`, a registry of pending connections (weak pointers, pruned lazily), and a destructor that abandons them |

**Re-applying after an upgrade.** Replace `httplib.h` with the new upstream version, then:

```bash
git apply --3way src/third_party/cpp-httplib/HTTPP_PATCHES.patch
```

(or apply the hunks by hand, searching for `httpp patch`). Then run the C++ tests
(`httpp_server.route_async_*`) and the Python tests (`test_async_*`).

## 2. Hooks for an external event loop

**Why.** cpp-httplib serves each open connection on one worker thread for its whole life, including
the time it sits idle waiting for the next keep-alive request. Idle connections therefore use up the
pool (a few dozen threads), and a request arriving behind them waits for their 5 s keep-alive timeout.
httpp now does the accepting and the idle watching itself, in `src/core/event_loop.cpp` (Asio: epoll,
kqueue or IOCP), and uses cpp-httplib only to parse, route and answer one request at a time. That needs a
few platform-independent hooks (this patch contains no platform code):

| API | Purpose |
|---|---|
| `ServeResult Server::serve_one(socket_t, size_t remaining)` | on a worker thread: serve exactly **one** request on an accepted socket, and report whether the connection may be reused (`keep`) or was taken over by a deferred response (`detached`). The caller keeps ownership of the socket otherwise. |
| `void Server::prepare_socket(socket_t)` | blocking mode, I/O timeouts and `TCP_NODELAY` for a socket the loop gives away |
| `Server::set_connection_releaser(fn)` | called when a deferred response completes and its connection may be kept alive; returns true if the loop took the socket back |
| `Server::set_external_loop(bool)` | see below |
| `DetachContext` | replaces the `bool *detached_out` parameter of `process_request` and also carries the remaining keep-alive budget |

**`set_external_loop` is essential.** The loop that writes streamed bodies (static files, content
providers) stops as soon as `svr_sock_ == INVALID_SOCKET`, which httplib reads as "the server is shutting
down". With an external loop `svr_sock_` is never set, so without this flag every streamed body is cut off
after the headers. (Ordinary string bodies are unaffected, which is why this was easy to miss.)
`tests/cpp/test_server.cpp` has a regression test with a 6 MB file.

httplib's own `listen()` / accept loop is left untouched and simply unused by httpp.

## Related non-patch changes
- The listening socket is created by `src/core/event_loop.cpp`: `SO_REUSEADDR` on POSIX (fast restart, but a
  second server cannot take a port that is already served, unlike cpp-httplib's default `SO_REUSEPORT`),
  `SO_EXCLUSIVEADDRUSE` on Windows, and the OS maximum backlog.
- With cpp-httplib's own accept loop, the default backlog of 128 made a burst of more than 128 new
  connections stall for tens of seconds (in a test, 44.9 s for the first request after 300 simultaneous
  connections). The Asio loop listens with the OS maximum, and the test `httpp_event_loop.*` /
  `idle_connections_do_not_occupy_worker_threads` cover it.
- Asio (1.34.2, standalone, header-only) is used unmodified. It is a git submodule at `src/third_party/asio`
  (pinned to `asio-1-34-2`); if the submodule is missing, CMake downloads it, pinned by SHA-256. See
  `src/third_party/README.md`.
