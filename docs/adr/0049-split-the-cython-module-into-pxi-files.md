# ADR-0049: Split the Cython module into `.pxi` files

- **Status:** accepted
- **Source:** `src/bindings/python/httpp_cy.pyx`, `src/bindings/python/cy/*.pxi`, `tests/python/test_cpp_python_parity.py`

## Context

`httpp_cy.pyx` had grown to about 2400 lines holding the C++ declarations, the blocking client, server request handling, WebSocket and loop glue, the server, downloads, URL and the async client.
Finding anything meant scrolling one file.

## Decision

The file keeps the cimports and the module docstring, then textually includes nine files from `cy/` in the original order: `_externs`, `_client`, `_request`, `_async_route`, `_server`,
`_download`, `_url_request`, `_async_client`, `_misc`. Cython's `include` is a textual paste, so the generated module is the same one: still one extension, no import cost, no change of
public names, and the order of definitions is exactly as before (declarations first, which later code relies on). The split is only a move of line ranges at top-level boundaries.
The public-header test (ADR-0017 rule 2) now scans the `.pxi` files too.

## Rejected alternatives

- Several extension modules with `.pxd` files: more cimport plumbing, more import time, and cross-module calls between the client, server and async classes that today share declarations.
- Moving thin Python wrappers into Cython: a separate question, decided by an audit of which Python files are thin or on the per-request path.
