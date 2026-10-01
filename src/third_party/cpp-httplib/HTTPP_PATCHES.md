# httpp patches to cpp-httplib

`httplib.h` here is cpp-httplib **0.54.1** plus the patch below. Every changed line in the header is
marked with `httpp patch`. `HTTPP_PATCHES.patch` is the same change as a diff against pristine 0.54.1.

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
- The connection is always closed after a detached response (no keep-alive).
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

## Related non-patch changes in `src/core/server.cpp`

- Sets `SO_REUSEADDR` only (not cpp-httplib's default `SO_REUSEPORT`) so a second server cannot silently
  share a port that is already served. Windows sets nothing.
