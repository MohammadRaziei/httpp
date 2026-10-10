#pragma once

// Internal, NOT installed. A non-blocking HTTP/1.1 client engine: Asio does the socket I/O,
// llhttp parses the response. One thread runs every exchange, so thousands of requests can be
// in flight without a thread each (unlike the blocking client, which needs one thread per request).
//
// Not done yet (see doc/status): TLS, redirects, proxy, auth/cookies, the keep-alive pool.
// Every exchange currently sends `Connection: close`.
//
// Like event_loop.hpp this header includes neither Asio nor cpp-httplib.

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
    std::string method = "GET";
    std::string target = "/"; // path and query, already percent-encoded
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    long timeout_sec = 0;               // per step (resolve, connect, write, each read); 0 = none
    std::size_t max_response_size = 0;  // body bytes; 0 = no limit
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

} // namespace httpp::detail
