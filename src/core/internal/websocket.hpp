#pragma once

// Internal, NOT installed. WebSocket (RFC 6455) as a `tunnel` of the engine (see ADR-0026): the handshake helpers, the frame codec and
// the session that turns frames into messages, answers ping and close, enforces the limits and queues what is sent. It needs neither
// sockets nor Asio, so the tests drive it with a fake tunnel_io.

#include "async_server.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace httpp::detail {

enum ws_opcode : unsigned { ws_continuation = 0, ws_text = 1, ws_binary = 2, ws_close = 8, ws_ping = 9, ws_pong = 10 };

// base64(SHA-1(key + the RFC's GUID)): the Sec-WebSocket-Accept value for a client key.
std::string ws_accept_key(const std::string& client_key);
// A Sec-WebSocket-Key is the base64 of exactly 16 bytes (24 characters, "==" at the end).
bool ws_key_is_valid(const std::string& client_key);
bool ws_utf8_valid(std::string_view text);
// Checks `text` as the start of a longer stream: false only when it can no longer become valid UTF-8; a character cut off at the end is
// fine for now. `complete` is how many leading bytes hold whole characters.
bool ws_utf8_scan(std::string_view text, std::size_t& complete);
// 1000-1003, 1007-1014 and 3000-4999 may be sent in a close frame.
bool ws_close_code_valid(unsigned code);
// One unmasked frame with FIN set (what a server sends).
std::string ws_frame(unsigned opcode, std::string_view payload);

enum class ws_status { ok, closed, full };

struct ws_limits {
    std::size_t max_message = 1u << 20;   // bytes of one message (all its fragments); more closes with 1009
    std::size_t max_buffered = 1u << 20;  // bytes queued for writing; a send over it is refused (ws_status::full)
    int ping_interval = 0;                // seconds of silence before a ping, and again before giving up; 0 = never
};

class ws_session;

struct ws_events {  // all run on the loop thread of the connection
    std::function<void()> on_open;
    std::function<void(std::string_view message, bool binary)> on_message;  // the view is valid during the call only
    std::function<void()> on_drain;                                         // the queue fell to half after a refused send
    std::function<void(std::uint16_t code, std::string_view reason)> on_close;  // exactly once; 1006 = no close frame
};

// What a handle needs from the connection, from any thread. Outlives the session (handles keep it); the session is reached through a weak
// pointer, so a handle used after the connection ended just reports `closed`.
class ws_shared {
public:
    ws_status send(unsigned opcode, std::string_view payload);  // text, binary, ping (pong is the session's own business)
    void close(std::uint16_t code, std::string_view reason);
    void pause_reading(bool pause);
    bool is_open() const { return open_.load(std::memory_order_acquire); }
    const std::shared_ptr<void>& user() const { return user_; }
    std::size_t buffered() const { return buffered_.load(std::memory_order_relaxed); }

private:
    friend class ws_session;
    void run(std::function<void(ws_session&, tunnel_io&)> fn);  // here if on the loop thread, else posted
    std::atomic<bool> open_{false};
    std::atomic<std::size_t> buffered_{0};   // bytes enqueued and not yet written
    std::atomic<bool> full_{false};          // a send was refused: tell the owner when there is room
    std::size_t max_buffered_ = 0;
    tunnel_post_fn post_;
    std::weak_ptr<ws_session> session_;
    std::thread::id loop_thread_;
    std::shared_ptr<void> user_;             // set before the session exists, never changed
};

class ws_session : public tunnel, public std::enable_shared_from_this<ws_session> {
public:
    static std::shared_ptr<ws_session> make(ws_events events, ws_limits limits, std::shared_ptr<void> user = {});
    ~ws_session() override;
    ws_session(const ws_session&) = delete;
    ws_session& operator=(const ws_session&) = delete;

    const std::shared_ptr<ws_shared>& shared() const { return shared_; }

    void on_open(tunnel_io& io, tunnel_post_fn post) override;
    void on_bytes(tunnel_io& io, const char* data, std::size_t n) override;
    void on_written(tunnel_io& io, std::size_t n) override;
    void on_idle(tunnel_io& io) override;
    void on_close() override;

private:
    friend class ws_shared;
    struct private_tag {};
public:
    ws_session(private_tag, ws_events events, ws_limits limits, std::shared_ptr<void> user);
private:
    void enqueue(tunnel_io& io, std::string frame);   // an already counted frame
    void start_close(tunnel_io& io, std::uint16_t code, std::string_view reason, bool wait_for_peer);
    void fail(tunnel_io& io, std::uint16_t code);     // protocol error: close frame, then drop
    bool parse_header(tunnel_io& io);
    void finish_frame(tunnel_io& io);
    void on_control(tunnel_io& io);

    ws_events events_;
    ws_limits limits_;
    std::shared_ptr<ws_shared> shared_;
    tunnel_io* io_ = nullptr;

    // parser
    enum class stage { header, payload } stage_ = stage::header;
    unsigned char head_[14];
    std::size_t head_have_ = 0, head_need_ = 2;
    unsigned opcode_ = 0;
    bool fin_ = false, masked_ = false;
    unsigned char mask_[4] = {0, 0, 0, 0};
    std::uint64_t remaining_ = 0, offset_ = 0;
    std::string msg_;          // the data message being assembled (all its fragments, unmasked)
    std::string ctrl_;         // the control frame being read
    bool in_message_ = false;  // a fragmented message is open
    bool msg_binary_ = false;
    std::size_t utf8_done_ = 0;     // text message: how many bytes of msg_ are already checked, whole characters only
    std::size_t frame_start_ = 0;   // where this frame's payload starts in msg_

    // state
    bool dead_ = false;             // a close was sent or received: bytes are ignored
    bool close_sent_ = false;
    bool ping_outstanding_ = false;
    bool closed_notified_ = false;
    bool got_close_ = false;
    std::uint16_t peer_code_ = 1006;
    std::string peer_reason_;
};

} // namespace httpp::detail
