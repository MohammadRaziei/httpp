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
- The base class is `httpp.responses.Response` only. `httpp.Response` is the CLIENT response, and changing that name would break the
  public API, so the top level exports just the concrete classes.

## Rejected alternatives
- Exporting the server base class as `httpp.Response`: collides with the client class.
- Handling `HTTPException` in C++: it is a Python exception, and the Python layer already owns exception handlers.

## Consequences
- Not done: an injected `response: Response` parameter (status and headers merged into the result), and `background=`
  (in `checklist.md`). `HTTPException` is also answered when a `before_request` hook raises it (the usual auth refusal).
- Verified: `tests/python/test_responses.py`, plus the 405 and symlink C++ tests in `tests/cpp/test_server_features.cpp`.

## Related
ADR-0007 (422 bodies), ADR-0017 (binding policy).
