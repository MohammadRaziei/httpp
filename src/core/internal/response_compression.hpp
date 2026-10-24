#pragma once

// Internal. Gzip for server answers (server::set_compression). One small class, so the rules live in one place:
// a body is compressed only when the setting is on, the client accepts gzip, the type is text-like, the body is
// big enough, nothing else already encoded it, and the result is really smaller. Everything else is untouched.

#include <cstddef>

#include "httpp/server.hpp"

namespace httpp::detail {

class response_compression {
public:
    // min_size 0 turns it off. level is zlib's 1 (fastest) to 9 (smallest); throws std::invalid_argument otherwise.
    void configure(std::size_t min_size, int level);
    bool enabled() const { return min_size_ != 0; }
    // True when apply() may spend CPU on this response (it is big enough); a cheap pre-check for callers
    // that must keep slow work off the I/O thread.
    bool worth_a_thread(const response& res) const { return enabled() && res.body.size() >= min_size_; }

    // Call with the final status, headers and body. May replace the body with its gzip form and add
    // Content-Encoding and Vary. Never touches a file, a stream or a body that is already encoded.
    void apply(const request& req, response& res) const;

private:
    std::size_t min_size_ = 0;
    int level_ = 1;
};

}  // namespace httpp::detail
