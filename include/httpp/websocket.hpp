#pragma once

#include "httpp/export.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace httpp {

struct request;   // server.hpp
struct response;  // client.hpp

namespace detail { class ws_shared; struct ws_upgrade; }

// One open WebSocket connection (RFC 6455), see server::route_ws. A cheap copyable handle: keep copies as long as you like, use them
// from any thread. After the connection ended every call reports `closed` or does nothing. Messages of one thread are sent in the
// order that thread sent them.
class websocket {
public:
    enum class status {
        ok,      // queued for sending
        closed,  // the connection is closing or gone: nothing was queued
        full     // the send queue holds max_buffered bytes: nothing was queued; ws_handler::on_drain says when there is room
    };

    websocket() = default;
    // Made by the server; not for callers.
    websocket(std::shared_ptr<detail::ws_shared> shared, std::shared_ptr<const request> upgrade)
        : shared_(std::move(shared)), upgrade_(std::move(upgrade)) {}

    HTTPP_API status send_text(std::string_view text) const;      // must be UTF-8 (the peer closes with 1007 otherwise)
    HTTPP_API status send_binary(std::string_view data) const;
    // A ping; the pong is not reported. At most 125 bytes, else std::invalid_argument.
    HTTPP_API status ping(std::string_view payload = {}) const;
    // Starts the closing handshake: sends a close frame, then waits up to 5 s for the peer's. Only the first call counts.
    // `code` must be 1000-1003, 1007-1014 or 3000-4999 (else std::invalid_argument); `reason` is cut to 123 bytes.
    HTTPP_API void close(std::uint16_t code = 1000, std::string_view reason = {}) const;
    // Stop and resume reading from the socket. A handler that hands messages to another thread calls pause_reading() when its own queue
    // is long and resume_reading() when it has drained: TCP then pushes back on the peer.
    HTTPP_API void pause_reading() const;
    HTTPP_API void resume_reading() const;

    HTTPP_API bool is_open() const;                // false once closing starts, from either side
    HTTPP_API std::size_t buffered_amount() const; // bytes queued for sending and not yet written to the socket
    // The upgrade request (path, query, path parameters, headers, cookies).
    const request& handshake() const { return *upgrade_; }
    explicit operator bool() const { return static_cast<bool>(shared_); }
    // What accept() was given as `user`, or null. For the callbacks, which run on the I/O thread: it is the way to find per-connection state
    // from a callback (the handle itself holds it, so it lives as long as any copy of the handle).
    HTTPP_API const std::shared_ptr<void>& user() const;

private:
    std::shared_ptr<detail::ws_shared> shared_;
    std::shared_ptr<const request> upgrade_;
};

// The answer to a WebSocket upgrade request that was accepted by the protocol rules (see ws_handler::on_handshake). Copyable and thread-safe;
// the first accept() or reject() wins and later calls do nothing. Dropping every copy without answering closes the connection without
// a response; one that is not answered within set_async_timeout() seconds gets a 504, like a route_async request.
class ws_acceptor {
public:
    ws_acceptor() = default;
    // Made by the server; not for callers.
    explicit ws_acceptor(std::shared_ptr<detail::ws_upgrade> upgrade) : upgrade_(std::move(upgrade)) {}

    // Sends the 101 and opens the connection: on_open, then messages, run on the I/O thread of the connection. `headers` go into the 101
    // (Sec-WebSocket-Protocol must be one the client offered, else std::invalid_argument is thrown and the client gets a 500).
    // `user` is kept by the connection's handles, see websocket::user().
    HTTPP_API void accept(std::vector<std::pair<std::string, std::string>> headers = {}, std::shared_ptr<void> user = {}) const;
    // Answers with an ordinary HTTP response instead (the error handler and the after hook apply, as for any answer).
    HTTPP_API void reject(const response& res) const;
    HTTPP_API void reject(int status = 403, std::string_view body = {}) const;

private:
    std::shared_ptr<detail::ws_upgrade> upgrade_;
};

// Everything below the callbacks that a WebSocket route needs. All callbacks run on the I/O thread of the connection (like route_io)
// and must return at once: never block, never wait for a lock that someone holds for long. To do slow work, hand the message to
// another thread and answer through the websocket handle.
struct ws_handler {
    // Optional; without it every valid upgrade is accepted. Runs on the I/O thread for an upgrade request that passed the protocol checks, and
    // must return at once, but the answer may come later, from any thread (look something up, ask a Python coroutine): call accept() or
    // reject() on the acceptor. This is where a browser app must check the Origin header (cross-site WebSocket hijacking).
    std::function<void(std::shared_ptr<const request>, ws_acceptor)> on_handshake;
    // Runs when the connection is open, before any message. Not called when the connection ended before it could open.
    std::function<void(const websocket&)> on_open;
    // The view is valid during the call only. `binary` is false for a text message (already checked to be UTF-8).
    std::function<void(const websocket&, std::string_view message, bool binary)> on_message;
    // The send queue fell to half of max_buffered after a send returned `full`.
    std::function<void(const websocket&)> on_drain;
    // Exactly once for every accepted upgrade, when the connection ends (also when it ended before on_open could run). `code` is the peer's
    // close code, 1005 if its close frame had none, 1006 if the connection ended without a close frame (dropped, timed out, server stopped).
    std::function<void(const websocket&, std::uint16_t code, std::string_view reason)> on_close;

    std::size_t max_message = 1u << 20;   // bytes of one message; a bigger one closes the connection with 1009
    std::size_t max_buffered = 1u << 20;  // bytes queued for sending; see websocket::status::full
    int ping_interval = 0;                // seconds of silence before a ping, and again before the connection is closed; 0 = off
};

} // namespace httpp
