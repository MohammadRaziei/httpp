# ADR-0047: One MIME table for the C++ and Python servers, and the exchange leak of `async_http`

- **Status:** accepted
- **Source:** `mime_type_for` in `include/httpp/server.hpp` and `src/core/server.cpp`, `mime_type_for` in `src/bindings/python/httpp_cy.pyx`, `file()` in
  `src/bindings/python/httpp/server.py`, `connection::drop_pending` and `exchange::fail` in `src/core/async_http.cpp`,
  `tests/cpp/test_mime.cpp`, `tests/python/test_responses.py`

## Context

Two separate things, both found by the checklist.

1. A mounted directory (`serve_directory`) chose the Content-Type from a table in `server.cpp`, but Python's `file()` and `FileResponse` guessed it with the standard
   `mimetypes` module. The two disagreed (`ico`, `gz`, `wav`, `yaml`) and the module also depends on the operating system's registry, so the same app could answer
   differently on Linux, macOS and Windows. The dual-API rule says the same design in both languages.
2. AddressSanitizer reported about 18 KB (7 allocations) of `async_http` exchange objects alive at exit.

## Decision

1. The C++ table is the only table. `httpp::mime_type_for(path)` is public (C++), bound in Cython as `httpp.httpp_cy.mime_type_for`, and used by `file()`, `FileResponse` and the
   `files=` parts of `TestClient`. The table gains `yaml` and `yml` (`application/yaml`, RFC 9512). Nothing else in it changed: `ico` stays `image/x-icon` (what browsers expect),
   `wav` stays `audio/wav`, `gz` stays `application/gzip`. An explicit `content_type` still wins. The `mimetypes` import is gone from both Python modules.
2. The leak was a reference cycle, found with valgrind on the test binary (`definitely lost`, 944 bytes direct and 17 275 indirect, allocated in `async_http::submit`):
   `exchange` owns its `connection`, and the connection's `pending` callback captures the exchange (`shared_from_this`). `exchange::fail` closed the connection but never
   released a callback that was still waiting, so any exchange given up while an operation was in flight (the engine being destroyed with requests pending, which the test
   `destroying_the_engine_cancels_pending_requests` does) was never freed. `exchange::fail` now calls `connection::drop_pending()`, which destroys the waiting callback without calling it.
   This is safe: `fail` is either called from inside the callback itself (then `pending` was already moved out and the call does nothing) or from the engine's teardown, where the exchange is
   still held by `live` until `complete()`.

## Rejected alternatives

- Keeping `mimetypes` and patching the differences in Python: a second table to keep in step, and still different per operating system.
- A `weak_ptr` in the callback instead of `shared_from_this`: the callback is what keeps a running exchange alive between operations, so it must own it.
- Clearing `pending` inside `connection::close()`: `close()` can be called from inside that very callback's call chain, and destroying a `std::function` while it runs is not safe.

## Consequences

- A file from `file()` has the same Content-Type as the same file from `serve_directory`, on every platform.
- `.yaml` files are served as `application/yaml` and, like every non-text type that is not on the inline list, as a download when opened in a browser.
- `ponytail:` no regression test for the leak itself (a unit test would need a hook that counts live exchanges). The check is a leak run: valgrind
  (`--leak-check=full`) on the suites `async_http`, `client_async`, `async_client_engine`, `client_async_redirects`, which reported `definitely lost` before and nothing after.
  Upgrade path: a debug counter of live exchanges and a test that expects zero after the engine is destroyed.
- Verified (Linux): 3 C++ tests of the table, 7 Python tests (the same file through both routes for six extensions, an explicit content type), the whole existing suites, valgrind as above.
- Not verified: the whole suite under ASan (the 18 KB report was the only leak noted before, but it was not run again with ASan here), Windows, macOS.
