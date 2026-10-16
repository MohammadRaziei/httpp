/* Appended to TF-PSA-Crypto's default configuration (TF_PSA_CRYPTO_USER_CONFIG_FILE, set in the root CMakeLists.txt).
 *
 * Thread safety. httpp runs TLS on many threads at once and TF-PSA-Crypto keeps process-wide state (the PSA random
 * generator, the key slots) that is only protected when the threading layer is on. Without it two handshakes can
 * draw from the DRBG at the same time (ThreadSanitizer: mbedtls_ctr_drbg_random_with_add), which can hand out the
 * same "random" bytes twice or break a handshake.
 *
 *  - POSIX: the pthread layer, nothing to register.
 *  - Windows has no pthread layer: the alt layer is plugged in at load time (src/core/mbedtls_threading.cpp) over
 *    SRWLOCK / CONDITION_VARIABLE, the cheapest native primitives. HTTPP_MBEDTLS_THREADING_ALT selects the alt
 *    layer on POSIX as well, over pthread, so CI can run the same registration code under ThreadSanitizer.
 */
#define MBEDTLS_THREADING_C
#if defined(_WIN32) || defined(HTTPP_MBEDTLS_THREADING_ALT)
#define MBEDTLS_THREADING_ALT
#else
#define MBEDTLS_THREADING_PTHREAD
#endif

/* The self tests (mbedtls_*_self_test) are not used, and while they are compiled in, the elliptic-curve code counts
 * its operations in process-wide counters (ecp.c: add_count, dbl_count, mul_count) that every handshake on every
 * thread increments with no lock: a data race by design, harmless to the result but it makes ThreadSanitizer useless
 * for the rest. Off, the counters and the self-test code are not built at all. */
#undef MBEDTLS_SELF_TEST
