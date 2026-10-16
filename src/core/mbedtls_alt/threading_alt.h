/* The mutex and condition variable types of the alt threading layer (see cmake/httpp_mbedtls_config.h). They are plain
 * C types because Mbed TLS itself is compiled as C. */
#ifndef HTTPP_MBEDTLS_THREADING_ALT_H
#define HTTPP_MBEDTLS_THREADING_ALT_H

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN /* keeps windows.h from pulling in winsock.h, which clashes with winsock2.h */
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
typedef SRWLOCK mbedtls_platform_mutex_t;
typedef CONDITION_VARIABLE mbedtls_platform_condition_variable_t;
#else
#include <pthread.h>
typedef pthread_mutex_t mbedtls_platform_mutex_t;
typedef pthread_cond_t mbedtls_platform_condition_variable_t;
#endif

#endif
