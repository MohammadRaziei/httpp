#pragma once

// Internal, NOT installed. TLS client over mbedtls for the async engine, with no socket inside:
// the caller moves ciphertext in (feed) and out (take_output), so it works with any I/O loop
// (here Asio). Includes neither Asio, cpp-httplib nor mbedtls headers.

#include <cstddef>
#include <memory>
#include <string>

namespace httpp::detail {

// Trust settings shared by many sessions: immutable after creation, safe to use from any thread.
struct tls_context;

// verify=false trusts anything. Otherwise ca_file (PEM) is the only trust anchor when given,
// else the system roots (loaded once per process). Returns null and fills `error` on failure.
std::shared_ptr<const tls_context> make_tls_context(bool verify, const std::string& ca_file, std::string& error);

class tls_session {
public:
    enum class result {
        ok,      // progress made (handshake done / plaintext produced / data queued)
        more,    // needs more ciphertext from the peer (and take_output() may have bytes to send)
        closed,  // the peer sent close_notify
        failed,  // see error()
    };

    tls_session(std::shared_ptr<const tls_context> ctx, const std::string& host);
    ~tls_session();
    tls_session(const tls_session&) = delete;
    tls_session& operator=(const tls_session&) = delete;

    bool ok() const;                 // false if the session could not be set up (see error())
    result handshake();              // call until ok; `more` means: send take_output(), read, feed()
    void feed(const char* data, std::size_t n);
    std::string take_output();       // ciphertext to put on the wire (may be empty)
    result write(const std::string& plain); // queues all of it as records; ok or failed
    result read(std::string& plain); // appends decrypted bytes: ok = got some, call again
    const std::string& error() const { return error_; }

private:
    struct impl;
    std::unique_ptr<impl> impl_;
    std::string error_;
};

} // namespace httpp::detail
