# ADR-0053: cpp-httplib is removed

- **Status:** accepted; supersedes the "cpp-httplib for the sync client" part of ADR-0003
- **Decided by:** the project owner
- **Source:** `src/core/curl.cpp`, `CMakeLists.txt`, `src/third_party/README.md`

## Context

After ADR-0051 (blocking client) and ADR-0052 (downloads) only `curl.cpp` still used `httplib.h`, a 22,600-line header compiled into the core.

## Decision

- `httpp::curl::request` becomes a thin face on `client::request` (same engine, same policy, no state of its own).
- `src/third_party/cpp-httplib/` and `src/core/internal/httplib_common.hpp` are deleted, with the include path, `CPPHTTPLIB_MBEDTLS_SUPPORT` and the sdist entry. mbedtls stays (it is `tls_session.cpp`'s), and so do the macOS
  CoreFoundation/Security link flags (the Keychain trust store is read by `tls_session.cpp`).
- The trust-store logic in `tls_session.cpp` follows cpp-httplib (MIT); its notice is in `src/third_party/README.md`.

## Consequences

- One HTTP implementation for server, client, async client and downloads; zero system dependencies is unchanged.
- Smaller binary and faster compile (no `httplib.h` translation units).
- Verified on Linux, Python 3.13: C++ 479/479; wheel built from scratch; Python 850 passed, 1 skipped (free-threaded only). Not verified: Windows, macOS, Python 3.14t, live sites.

## Related

ADR-0003, ADR-0051, ADR-0052
