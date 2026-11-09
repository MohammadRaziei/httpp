# ADR-0048: WebSocket routes on routers, and dependencies on WebSocket routes

- **Status:** accepted
- **Source:** `router::route_ws` / `ws_routes` in `include/httpp/router.hpp` and `src/core/router.cpp`, `APIRouter.websocket` in `src/bindings/python/httpp/_router.py`,
  `_adapt_ws` and `Server.websocket` in `src/bindings/python/httpp/server.py`, `Ctx` and `Plan.wants_websocket` in `src/bindings/python/httpp/_params.py`,
  `tests/cpp/test_router.cpp`, `tests/python/test_websocket_router.py`

## Context

A WebSocket route could only be registered on a `Server`, so an app split into routers had to keep its WebSocket endpoints outside them, and a WebSocket handler could not use
`Depends` (authentication, a database session, a shared limit), which FastAPI users expect.

## Decision

1. **Routers.** `router::route_ws(path, ws_handler)` (C++) and `@router.websocket(path, **options)` (Python) record the route. `include` joins prefixes exactly as for HTTP routes (outer prefix,
   router prefix, path), and `server::include` registers each one with `route_ws` / `Server.websocket`. Python options are checked when the route is declared, with the same rules as
   `Server.websocket` (`_check_ws_options`). Tags do not apply (WebSocket routes are not in OpenAPI).
2. **Dependencies (Python only: C++ has no dependency system).** Dependencies are solved before the handler in a per-connection `AsyncExitStack`, so `yield` dependencies are closed after the
   handler ends, also after an exception, last opened first. A validation error in a dependency refuses the upgrade with 422 (the same body as path errors); an `HTTPException` refuses it with
   its status. A dependency may take `websocket` (by name or annotation) to accept or close the connection itself. Using such a dependency on an HTTP route is a `TypeError` at registration.
3. Still refused on WebSocket routes: `response` and background tasks (nothing to attach them to). WebSocket routes stay out of the OpenAPI document, which cannot describe them in 3.1.

## Rejected alternatives

- Solving dependencies in the C++ handshake callback: the work is Python and async, and the Python task already answers the upgrade (accept / reject).
- A separate `WebSocketDepends`: one marker is enough, the connection is injected by name like `request`.
