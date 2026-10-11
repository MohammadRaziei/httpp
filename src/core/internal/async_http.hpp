#pragma once

// Internal, NOT installed. A non-blocking HTTP/1.1 client engine: Asio does the socket I/O,
// llhttp parses the response. One thread runs every exchange, so thousands of requests can be
// in flight without a thread each (unlike the blocking client, which needs one thread per request).
//
// HTTPS goes through tls_session (mbedtls) on the same loop. Connections can be kept alive and
// reused (job.keep_alive), and an HTTP proxy is supported (plain http through it, https via CONNECT).
// Redirects, auth and cookies are NOT here: they are policy for the layer above (client.cpp), which
// builds one async_job per hop.
//
// Like async_server.hpp this header includes neither Asio nor cpp-httplib.

#include "httpp/client.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace httpp::detail {

struct async_job {
    std::string host;
    int port = 80;
    bool tls = false;                   // https: port is usually 443
    bool verify = true;                 // check the server certificate and host name
    std::string ca_file;                // PEM; only trust this (default: the system roots)
    std::string method = "GET";
    std::string target = "/"; // path and query, already percent-encoded
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    long timeout_sec = 0;               // per step (resolve, connect, write, each read); 0 = none
    std::size_t max_response_size = 0;  // body bytes; 0 = no limit
    // Keep the connection for the next request to the same place, and reuse an idle one if there is one.
    // A reused connection the server had already closed is retried once on a fresh one, but only for
    // idempotent methods (GET, HEAD, OPTIONS, PUT, DELETE) and only if no response byte arrived.
    bool keep_alive = false;
    // HTTP proxy. http:// targets are sent in absolute form; https:// targets are tunnelled with CONNECT.
    std::string proxy_host;
    int proxy_port = 0;
    std::string proxy_authorization;    // value for Proxy-Authorization (e.g. "Basic ..."), or empty
};

class async_http {
public:
    // Runs on the loop thread: must not block (fulfil a promise, or hand work to a pool).
    using done_fn = std::function<void(response)>;

    async_http();
    ~async_http(); // pending requests finish with error_kind::canceled
    async_http(const async_http&) = delete;
    async_http& operator=(const async_http&) = delete;

    // Thread-safe. `done` is called exactly once. Never throws for network trouble: the
    // response carries status 0 and `error` instead (same contract as httpp::client).
    void submit(async_job job, done_fn done);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// One engine (one loop thread) for the whole process, created on first use. Intentionally never
// destroyed: joining the loop thread during static destruction (or while an embedding interpreter is
// shutting down) is a classic way to hang at exit. The OS reclaims it with the process.
async_http& shared_async_http();

} // namespace httpp::detail
