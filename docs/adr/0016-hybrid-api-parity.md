# ADR-0016: Hybrid API parity: C++ and Python expose equivalent public APIs

- **Status:** accepted as a goal; the gaps below are open
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

## Current state

Equivalent today: `server` (routes, `route_async`, `serve_directory`, bind/listen/stop, hooks, all settings, `mount_swagger_ui`),
`client` and `async_client` (HTTP methods, `fetch`; the C++ setters are constructor arguments in Python), `url`, `log`, `progress::bar`.

Missing in Python:

- `Download.sha256`, `Download.sha256_url`, `Download.set_timeout`, `Download.set_max_wait`, and the `sha256` field of `DownloadResult`
- `sha256_hex`, `sha256_file`, `parse_sha256_checksum`
- the curl-like `httpp::client::request` builder under the same name (Python has `httpp.Request`, with a different interface)
- public access to `route_io` and `route_shared` (only the internal `add_route` and `add_loop_route` exist)

Missing in C++ (by design the FastAPI-like layer): OpenAPI generation, `Query/Path/Header/Cookie/Body/Depends`, models/validation,
`on_startup`/`on_shutdown`, `run()` and the `httpp run` CLI.

## Consequences

- There is no automated test that every C++ public method has a Python counterpart, so a gap can appear without anyone noticing.
  The checklist asks for one (a test that parses the headers and compares names, with an allow-list).
- The comparison is by name and signature; a method with a different name may be equivalent and not recognized.
- The open gaps are tracked in `checklist.md`, section "Hybrid API parity".

## Related

ADR-0001, ADR-0008, ADR-0014
