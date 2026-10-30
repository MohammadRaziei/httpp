# Hand-off 14

State: WebSocket is done in the C++ core (ADR-0026) and in Python (ADR-0027). handoff_13.md still describes the old synchronous
`on_handshake`; ADR-0027 and the headers are the truth (`ws_acceptor`, `websocket::user()`, `on_close` exactly once).

Verified: C++ 374 tests (43 session + 35 server WebSocket), TSan clean on the WebSocket tests before the async handshake rework (not re-run after it),
Python 652 passed (3.14) and 651 passed (3.14t), 27 of them WebSocket.

Not verified: Autobahn, Windows and macOS, a WebSocket benchmark, memory with many idle connections, TSan on the whole suite after the rework,
the Python glue under a free-threaded stress of many connections for long.

Python glue: `httpp_cy.pyx` (`httpp_py_wsconn` mailbox, `_WsConn`, `_start_ws`, `_ws_wake`, `Server._add_ws_route`), `httpp/_websocket.py`
(`WebSocket`, `WebSocketDisconnect`), `server.py` (`_adapt_ws`, `Server.websocket`, `_origin_ok`), `_params.py` (`websocket_name`).

Next: Autobahn (needs the owner's machine), WebSocket benchmark, `APIRouter.websocket`, dependencies for WebSocket routes, OpenAPI note.
Open decision from before: the free-threaded `io_threads` default.
