# ADR-0010: Mbed TLS thread safety: `MBEDTLS_THREADING_C` plus an alt layer on Windows

- **Status:** accepted
- **Source:** `cmake/httpp_mbedtls_config.h`, `src/core/mbedtls_threading.cpp`, `src/core/mbedtls_alt/`

## Context

Under ThreadSanitizer (Linux) it turned out that `MBEDTLS_THREADING_C` was off and PSA's global DRBG was shared between concurrent handshakes
without a lock. This was not only test flakiness: two handshakes could receive the same random bytes.

## Decision

- Configuration through `TF_PSA_CRYPTO_USER_CONFIG_FILE` (the submodule is not touched): `MBEDTLS_THREADING_C` with pthread on POSIX.
- On Windows an alt layer built on `SRWLOCK`/`CONDITION_VARIABLE`, registered when the library is loaded.
  `-DHTTPP_MBEDTLS_THREADING_ALT=ON` enables the same layer on POSIX too, so it can be tested under TSAN.
- `MBEDTLS_SELF_TEST` was turned off: the `add_count/dbl_count/mul_count` counters in `ecp.c` exist only for the self-test and are
  incremented without a lock (a designed race).

## Consequences

- Under TSAN, with pthread and with alt, all 86 `async_*` tests pass and there is no mbedtls race; `many_concurrent_handshakes`
  passed 30 times in a row on Linux.
- `mbedtls_threading_free_alt()` is never called (`ponytail:`; upgrade path: an explicit shutdown after joining the TLS threads).
- CI: the C++ tests (including `many_concurrent_handshakes`) run and pass on Linux and macOS. On Windows the SRWLOCK layer is built into every
  wheel and the wheel's Python tests pass, but the C++ tests do not run on Windows CI and the Python TLS tests
  (`needs_openssl`) are skipped on Windows ([ADR-0015](0015-testing-and-ci.md)).

## Related

ADR-0003
