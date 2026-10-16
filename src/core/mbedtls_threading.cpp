// Registers the alt threading layer with Mbed TLS when the build selected it (Windows, or HTTPP_MBEDTLS_THREADING_ALT).
// On POSIX the pthread layer needs nothing: this file is then empty.
#include <mbedtls/build_info.h>

#if defined(MBEDTLS_THREADING_ALT)
#include <mbedtls/threading.h>

namespace {

#if defined(_WIN32)
int mutex_init(mbedtls_platform_mutex_t* m) { InitializeSRWLock(m); return 0; }
void mutex_destroy(mbedtls_platform_mutex_t*) {}
int mutex_lock(mbedtls_platform_mutex_t* m) { AcquireSRWLockExclusive(m); return 0; }
int mutex_unlock(mbedtls_platform_mutex_t* m) { ReleaseSRWLockExclusive(m); return 0; }
int cond_init(mbedtls_platform_condition_variable_t* c) { InitializeConditionVariable(c); return 0; }
void cond_destroy(mbedtls_platform_condition_variable_t*) {}
int cond_signal(mbedtls_platform_condition_variable_t* c) { WakeConditionVariable(c); return 0; }
int cond_broadcast(mbedtls_platform_condition_variable_t* c) { WakeAllConditionVariable(c); return 0; }
int cond_wait(mbedtls_platform_condition_variable_t* c, mbedtls_platform_mutex_t* m) {
    return SleepConditionVariableSRW(c, m, INFINITE, 0) ? 0 : MBEDTLS_ERR_THREADING_USAGE_ERROR;
}
#else
int mutex_init(mbedtls_platform_mutex_t* m) { return pthread_mutex_init(m, nullptr) == 0 ? 0 : MBEDTLS_ERR_THREADING_USAGE_ERROR; }
void mutex_destroy(mbedtls_platform_mutex_t* m) { pthread_mutex_destroy(m); }
int mutex_lock(mbedtls_platform_mutex_t* m) { return pthread_mutex_lock(m) == 0 ? 0 : MBEDTLS_ERR_THREADING_USAGE_ERROR; }
int mutex_unlock(mbedtls_platform_mutex_t* m) { return pthread_mutex_unlock(m) == 0 ? 0 : MBEDTLS_ERR_THREADING_USAGE_ERROR; }
int cond_init(mbedtls_platform_condition_variable_t* c) { return pthread_cond_init(c, nullptr) == 0 ? 0 : MBEDTLS_ERR_THREADING_USAGE_ERROR; }
void cond_destroy(mbedtls_platform_condition_variable_t* c) { pthread_cond_destroy(c); }
int cond_signal(mbedtls_platform_condition_variable_t* c) { return pthread_cond_signal(c) == 0 ? 0 : MBEDTLS_ERR_THREADING_USAGE_ERROR; }
int cond_broadcast(mbedtls_platform_condition_variable_t* c) { return pthread_cond_broadcast(c) == 0 ? 0 : MBEDTLS_ERR_THREADING_USAGE_ERROR; }
int cond_wait(mbedtls_platform_condition_variable_t* c, mbedtls_platform_mutex_t* m) {
    return pthread_cond_wait(c, m) == 0 ? 0 : MBEDTLS_ERR_THREADING_USAGE_ERROR;
}
#endif

// Mbed TLS wants this called once, before any other of its functions. A static object in the library does it when the
// library is loaded, before main() and before any caller can reach Mbed TLS through httpp, with nothing to remember
// in every entry point. Nothing here waits or loads a library, so it is also safe under the Windows loader lock.
// ponytail: mbedtls_threading_free_alt() is never called: the global mutexes live as long as the process. Upgrade
// path: call it from an explicit httpp shutdown, once every thread that may use TLS has been joined.
struct registrar {
    registrar() {
        mbedtls_threading_set_alt(mutex_init, mutex_destroy, mutex_lock, mutex_unlock, cond_init, cond_destroy,
                                  cond_signal, cond_broadcast, cond_wait);
    }
} registrar_instance;

} // namespace
#endif
