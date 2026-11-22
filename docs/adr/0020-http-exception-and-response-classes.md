# ADR-0020: HTTPException and response classes

## Status
Accepted (the owner asked for them; the design below was reported with the change).

## Context
Handlers could only return a body or `(body, status, headers)`, and errors were hand-made bodies. FastAPI code uses
`HTTPException` and `JSONResponse`, `RedirectResponse` and friends.

## Decision
- New module `httpp/responses.py`: `HTTPException(status_code, detail=None, headers=None)` and `Response`, `JSONResponse`,
  `PlainTextResponse`, `HTMLResponse`, `RedirectResponse`, `StreamingResponse`, `FileResponse`. `httpp/status.py` has the
  `HTTP_404_NOT_FOUND` style constants, generated from `http.HTTPStatus`.
- A response object is turned into the existing `(body, status, headers)` tuple at the top of `server._to_response`, so there is
  one conversion path and no new core feature. `StreamingResponse` and `FileResponse` reuse the generator and `file()` support.
- `HTTPException` is answered by a default exception handler (`{"detail": detail}`, its status and headers) that is used when the
  app has no handler for it; `@app.exception_handler(HTTPException)` overrides it. It works in sync and async routes and in
  dependencies (the `yield` teardown still runs) and in `before_request` hooks.
- A returned response object skips `response_model`, like FastAPI.
- **C++ twin, added later at the owner's request:** `httpp::http_exception(status, detail = {}, headers = {})` in `httpp/server.hpp`, derived from `std::runtime_error` (`what()` is `"404: no such user"`, like Python's `str`). The server answers it before the exception handler runs, with `{"detail": detail}` as `application/json`, its status and headers, and `detail` defaults to the reason phrase ("" when the status has none); a status outside 100 to 599 throws `std::invalid_argument`, as Python raises `ValueError`. The one function `exception_response` serves the sync handlers, `get_async` handlers and a websocket `on_handshake`. A C++ exception handler runs after it, sees the exception and may change the answer; one that ignores it leaves the answer. It is a separate class: the Python one stays pure Python.
- The base class is `httpp.responses.Response` only. `httpp.Response` is the CLIENT response, and changing that name would break the
  public API, so the top level exports just the concrete classes.

## Rejected alternatives
- Exporting the server base class as `httpp.Response`: collides with the client class.
- Handling the Python `HTTPException` in C++: it is a Python exception, and the Python layer already owns exception handlers. (A C++ class of its own was added later, see Decision.)

## Consequences
- Not done: an injected `response: Response` parameter (status and headers merged into the result), and `background=`
  (in `checklist.md`). `HTTPException` is also answered when a `before_request` hook raises it (the usual auth refusal).
- Verified: `tests/python/test_responses.py`, plus the 405 and symlink C++ tests in `tests/cpp/test_server_features.cpp`. The C++ twin: `tests/cpp/test_http_exception.cpp`, the websocket handshake case in `tests/cpp/test_server_websocket.cpp`, and the `errors` example in `tests/python/test_examples.py` (both languages).
- Differences from Python that were left: the C++ body is `{"detail":"..."}` without the space that `json.dumps` adds, and non-ASCII text in `detail` is sent as UTF-8, not as `\uXXXX` escapes. Both are the same JSON.

## Related
ADR-0007 (422 bodies), ADR-0016 (binding policy).
