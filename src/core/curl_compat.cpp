#include "httpp/curl_compat.h"
#include "httpp/client.hpp" // reused directly — no duplicated HTTP logic here

#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

// The real CURL struct definition (opaque to callers via the forward
// declaration in curl_compat.h). It only ACCUMULATES options set via
// curl_easy_setopt(); the actual request is built and run through
// httpp::client::request inside curl_easy_perform(), which is the same
// class the plain C++ API (httpp::client::request) already uses — reusing
// it here means there's exactly one implementation of "run an HTTP
// request", not two.
struct CURL {
    std::string url;
    std::string method;
    std::string post_fields;
    bool has_post_fields = false;
    struct curl_slist* headers = nullptr;
    curl_write_callback write_cb = nullptr;
    void* write_data = nullptr;
    curl_header_callback header_cb = nullptr;
    void* header_data = nullptr;
    std::string user_agent;
    long timeout_seconds = 0;
    bool follow_location = false;

    long last_response_code = 0;
    std::string last_content_type;
};

namespace {

char* dup_cstr(const char* s) {
    const std::size_t n = std::strlen(s) + 1;
    char* p = new char[n];
    std::memcpy(p, s, n);
    return p;
}

} // namespace

extern "C" {

CURL* curl_easy_init(void) {
    return new CURL();
}

void curl_easy_cleanup(CURL* curl) {
    delete curl;
}

CURLcode curl_easy_setopt(CURL* curl, CURLoption option, ...) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;

    va_list args;
    va_start(args, option);
    CURLcode rc = CURLE_OK;

    switch (option) {
        case CURLOPT_URL: {
            const char* v = va_arg(args, const char*);
            curl->url = v ? v : "";
            break;
        }
        case CURLOPT_CUSTOMREQUEST: {
            const char* v = va_arg(args, const char*);
            curl->method = v ? v : "";
            break;
        }
        case CURLOPT_HTTPGET:
            (void)va_arg(args, long);
            curl->method = "GET";
            break;
        case CURLOPT_POST:
            (void)va_arg(args, long);
            if (curl->method.empty()) curl->method = "POST";
            break;
        case CURLOPT_POSTFIELDS: {
            const char* v = va_arg(args, const char*);
            curl->post_fields = v ? v : "";
            curl->has_post_fields = true;
            break;
        }
        case CURLOPT_POSTFIELDSIZE: {
            long n = va_arg(args, long);
            if (n >= 0 && static_cast<std::size_t>(n) <= curl->post_fields.size()) {
                curl->post_fields.resize(static_cast<std::size_t>(n));
            }
            break;
        }
        case CURLOPT_HTTPHEADER:
            curl->headers = va_arg(args, struct curl_slist*);
            break;
        case CURLOPT_WRITEFUNCTION:
            curl->write_cb = va_arg(args, curl_write_callback);
            break;
        case CURLOPT_WRITEDATA:
            curl->write_data = va_arg(args, void*);
            break;
        case CURLOPT_HEADERFUNCTION:
            curl->header_cb = va_arg(args, curl_header_callback);
            break;
        case CURLOPT_HEADERDATA:
            curl->header_data = va_arg(args, void*);
            break;
        case CURLOPT_USERAGENT: {
            const char* v = va_arg(args, const char*);
            curl->user_agent = v ? v : "";
            break;
        }
        case CURLOPT_TIMEOUT:
            curl->timeout_seconds = va_arg(args, long);
            break;
        case CURLOPT_FOLLOWLOCATION:
            curl->follow_location = va_arg(args, long) != 0;
            break;
        case CURLOPT_VERBOSE:
            (void)va_arg(args, long); // accepted, not acted on
            break;
        default:
            rc = CURLE_UNKNOWN_OPTION;
            break;
    }

    va_end(args);
    return rc;
}

CURLcode curl_easy_perform(CURL* curl) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;
    if (curl->url.empty()) return CURLE_UNSUPPORTED_PROTOCOL;

    httpp::client::request req(curl->url);
    if (!curl->method.empty()) req.method(curl->method);
    if (curl->has_post_fields) req.data(curl->post_fields);
    if (!curl->user_agent.empty()) req.header("User-Agent", curl->user_agent);
    if (curl->timeout_seconds > 0) req.timeout(curl->timeout_seconds);
    req.follow_redirects(curl->follow_location);

    for (auto* h = curl->headers; h != nullptr; h = h->next) {
        if (!h->data) continue;
        const std::string line(h->data);
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        const auto first_non_space = value.find_first_not_of(' ');
        value = (first_non_space == std::string::npos) ? "" : value.substr(first_non_space);
        req.header(name, value);
    }

    const httpp::response res = req.run();
    curl->last_response_code = res.status;
    curl->last_content_type = res.header("Content-Type");

    if (res.status == 0) {
        return CURLE_COULDNT_CONNECT;
    }

    if (curl->header_cb) {
        for (const auto& [name, value] : res.headers) {
            const std::string line = name + ": " + value + "\r\n";
            curl->header_cb(const_cast<char*>(line.data()), 1, line.size(), curl->header_data);
        }
    }

    if (curl->write_cb && !res.body.empty()) {
        curl->write_cb(const_cast<char*>(res.body.data()), 1, res.body.size(), curl->write_data);
    }

    return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL* curl, CURLINFO info, ...) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;

    va_list args;
    va_start(args, info);
    CURLcode rc = CURLE_OK;

    switch (info) {
        case CURLINFO_RESPONSE_CODE: {
            long* out = va_arg(args, long*);
            if (out) *out = curl->last_response_code;
            break;
        }
        case CURLINFO_CONTENT_TYPE: {
            const char** out = va_arg(args, const char**);
            if (out) *out = curl->last_content_type.empty() ? nullptr : curl->last_content_type.c_str();
            break;
        }
        default:
            rc = CURLE_UNKNOWN_OPTION;
            break;
    }

    va_end(args);
    return rc;
}

const char* curl_easy_strerror(CURLcode code) {
    switch (code) {
        case CURLE_OK: return "No error";
        case CURLE_UNSUPPORTED_PROTOCOL: return "Unsupported protocol";
        case CURLE_COULDNT_CONNECT: return "Couldn't connect to server";
        case CURLE_HTTP_RETURNED_ERROR: return "HTTP returned error";
        case CURLE_WRITE_ERROR: return "Write error";
        case CURLE_OUT_OF_MEMORY: return "Out of memory";
        case CURLE_OPERATION_TIMEDOUT: return "Operation timed out";
        case CURLE_BAD_FUNCTION_ARGUMENT: return "Bad function argument";
        case CURLE_UNKNOWN_OPTION: return "Unknown option";
        default: return "Unknown error";
    }
}

struct curl_slist* curl_slist_append(struct curl_slist* list, const char* value) {
    auto* node = new curl_slist();
    node->data = value ? dup_cstr(value) : nullptr;
    node->next = nullptr;

    if (!list) return node;
    auto* tail = list;
    while (tail->next) tail = tail->next;
    tail->next = node;
    return list;
}

void curl_slist_free_all(struct curl_slist* list) {
    while (list) {
        auto* next = list->next;
        delete[] list->data;
        delete list;
        list = next;
    }
}

CURLcode curl_global_init(long) {
    return CURLE_OK;
}

void curl_global_cleanup(void) {}

} // extern "C"
