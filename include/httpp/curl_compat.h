#pragma once

/*
 * A small, drop-in-ish subset of libcurl's `curl_easy_*` C API — meant to
 * let existing code migrate to httpp with (ideally) just a header swap:
 *
 *     -#include <curl/curl.h>
 *     +#include <httpp/curl_compat.h>
 *
 * This is NOT a full libcurl replacement: only the commonly used easy-API
 * subset below is implemented, and every curl_easy_setopt() call is
 * forwarded to (and reuses the exact same code as) httpp::client::request
 * (see include/httpp/client.hpp, src/core/curl_compat.cpp) — there is no
 * separate/duplicated HTTP implementation here.
 *
 * Covered: CURLOPT_URL, CURLOPT_CUSTOMREQUEST, CURLOPT_HTTPGET,
 * CURLOPT_POST, CURLOPT_POSTFIELDS(+SIZE), CURLOPT_HTTPHEADER,
 * CURLOPT_WRITEFUNCTION/DATA, CURLOPT_HEADERFUNCTION/DATA,
 * CURLOPT_USERAGENT, CURLOPT_TIMEOUT, CURLOPT_FOLLOWLOCATION, curl_slist_*,
 * curl_easy_getinfo (CURLINFO_RESPONSE_CODE, CURLINFO_CONTENT_TYPE),
 * curl_easy_strerror, curl_global_init/cleanup.
 *
 * NOT covered (no such option exists here — curl_easy_setopt() returns
 * CURLE_UNKNOWN_OPTION for anything not listed above): proxies, cookies,
 * TLS/certificate options, auth (basic/digest/bearer), multipart forms,
 * multi/share handles, and everything else in real libcurl's ~300
 * CURLOPT_* options.
 */

#include "httpp/export.hpp"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CURL CURL;

typedef enum {
    CURLE_OK = 0,
    CURLE_UNSUPPORTED_PROTOCOL,
    CURLE_COULDNT_CONNECT,
    CURLE_HTTP_RETURNED_ERROR,
    CURLE_WRITE_ERROR,
    CURLE_OUT_OF_MEMORY,
    CURLE_OPERATION_TIMEDOUT,
    CURLE_BAD_FUNCTION_ARGUMENT,
    CURLE_UNKNOWN_OPTION
} CURLcode;

typedef enum {
    CURLOPT_URL,
    CURLOPT_CUSTOMREQUEST,
    CURLOPT_HTTPGET,
    CURLOPT_POST,
    CURLOPT_POSTFIELDS,
    CURLOPT_POSTFIELDSIZE,
    CURLOPT_HTTPHEADER,
    CURLOPT_WRITEFUNCTION,
    CURLOPT_WRITEDATA,
    CURLOPT_HEADERFUNCTION,
    CURLOPT_HEADERDATA,
    CURLOPT_USERAGENT,
    CURLOPT_TIMEOUT,
    CURLOPT_FOLLOWLOCATION,
    CURLOPT_VERBOSE /* accepted, has no effect */
} CURLoption;

typedef enum {
    CURLINFO_RESPONSE_CODE,
    CURLINFO_CONTENT_TYPE /* returns const char*, or NULL if absent */
} CURLINFO;

typedef size_t (*curl_write_callback)(char* ptr, size_t size, size_t nmemb, void* userdata);
/* Called once per response header line, formatted "Name: value\r\n" —
 * matches real libcurl's curl_write_callback signature exactly, since
 * that's what CURLOPT_HEADERFUNCTION expects there too. */
typedef curl_write_callback curl_header_callback;

struct curl_slist {
    char* data;
    struct curl_slist* next;
};

HTTPP_API CURL* curl_easy_init(void);
HTTPP_API void curl_easy_cleanup(CURL* curl);
HTTPP_API CURLcode curl_easy_setopt(CURL* curl, CURLoption option, ...);
HTTPP_API CURLcode curl_easy_perform(CURL* curl);
HTTPP_API CURLcode curl_easy_getinfo(CURL* curl, CURLINFO info, ...);
HTTPP_API const char* curl_easy_strerror(CURLcode code);

HTTPP_API struct curl_slist* curl_slist_append(struct curl_slist* list, const char* value);
HTTPP_API void curl_slist_free_all(struct curl_slist* list);

#define CURL_GLOBAL_DEFAULT 0L
HTTPP_API CURLcode curl_global_init(long flags);
HTTPP_API void curl_global_cleanup(void);

#ifdef __cplusplus
}
#endif
