# ADR-0016: Hybrid API parity: C++ and Python expose equivalent public APIs

- **Status:** accepted; the parity test enforces names, the gaps below are open
- **Date:** 2026-10-18
- **Source:** comparison of `include/httpp/*.hpp` with `dir(httpp)` and `src/bindings/python/httpp_cy.pyx` (names and signatures, not behavior)

## Context

httpp is a hybrid library. The same capability should be reachable from C++ and from Python under the same name and with the same
options, so a user can move between the two without surprises. The question "are the two APIs equivalent today?" had no recorded answer.

## Decision

- **Parity is the goal** for the network and server core: `server`, `client`, `async_client`, `download`, `url`, `log`, `progress`, `sha256`.
- The FastAPI-like layer (parameter declarations, models, OpenAPI, `httpp run`) is **Python by design**. It is not required to exist in C++,
  but any gap there is written down instead of left implicit.
- A new public C++ method lands together with its Python binding, or with an entry in an explicit allow-list of intentional differences.
- `tests/python/test_cpp_python_parity.py` enforces this: it parses the `HTTPP_API` declarations in `include/httpp/*.hpp` and compares names with the Python objects.
  Intentional differences are listed with a reason: `ALIASES` (`del` is `delete`, `send` is `request`, `is_finished` is the property `finished`, ...),
  `CTOR_ARGS` (C++ setters of `client`/`async_client` that are constructor arguments in Python, and the test constructs them) and `KNOWN_GAPS`
  (open gaps; an entry that stops being a gap fails the test). A new C++ class without a `TARGETS` entry fails it too.

## Current state

Equivalent: `server` (routes, `route_async`, `serve_directory`, bind/listen/stop, hooks, all settings, `mount_swagger_ui`), `client` and `async_client`
(HTTP methods, `fetch`; the setters are constructor arguments in Python), `download` (including `sha256`, `sha256_url`, `set_timeout`,
`set_max_wait` taking seconds, and `DownloadResult.sha256`), `sha256_hex`, `sha256_file`, `parse_sha256_checksum`, `url`, `log`, `progress::bar`.
The first run of the parity test found one more gap that nobody had listed: `Client(keep_alive=...)`; it was added.

Still missing in Python (`KNOWN_GAPS`):

- public access to `route_io` and `route_shared` (only the internal `add_loop_route` and `add_route` exist)
- `find_swagger_ui_asset`, a C++ lookup helper that is probably not meant for users

`httpp::curl::request` is covered by name through `httpp.Request`, which has the same builder methods; it is not the same class.

Missing in C++ (by design the FastAPI-like layer): OpenAPI generation, `Query/Path/Header/Cookie/Body/Depends`, models/validation,
`on_startup`/`on_shutdown`, `run()` and the `httpp run` CLI.

## Consequences

- The comparison is by name only (and constructor keywords for the setters); it says nothing about behavior or signatures. Behavior is covered by ordinary tests.
- The header parser is regex based (`ponytail:`); a declaration written in an unusual way could be missed. Upgrade path: libclang.
- The test runs in the wheel CI only when the C++ headers are next to the tests (they are, in the repository checkout); otherwise it is skipped.
- The open gaps are tracked in `checklist.md`, section "Hybrid API parity".

## Related

ADR-0001, ADR-0008, ADR-0014
