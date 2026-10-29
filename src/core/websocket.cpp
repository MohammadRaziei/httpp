#include "internal/websocket.hpp"
#include "internal/log.hpp"

#include <psa/crypto.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace httpp::detail {
namespace {

constexpr std::size_t max_control_payload = 125;
constexpr std::size_t max_close_reason = 123;                 // a close frame holds a 2 byte code and this much text
constexpr std::chrono::milliseconds close_wait{5000};         // how long we wait for the peer's close after sending ours
constexpr std::size_t keep_capacity = 64 * 1024;              // a message buffer bigger than this is released after the message

const char b64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const unsigned char* d, std::size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        const unsigned v = (d[i] << 16) | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out += b64_alphabet[(v >> 18) & 63];
        out += b64_alphabet[(v >> 12) & 63];
        out += i + 1 < n ? b64_alphabet[(v >> 6) & 63] : '=';
        out += i + 2 < n ? b64_alphabet[v & 63] : '=';
    }
    return out;
}

bool b64_char(char c) { return std::strchr(b64_alphabet, c) != nullptr && c != '\0'; }

std::string close_body(std::uint16_t code, std::string_view reason) {
    if (code == 0) return {};
    std::string p;
    p.push_back(static_cast<char>(code >> 8));
    p.push_back(static_cast<char>(code & 0xff));
    p.append(reason.substr(0, max_close_reason));
    return p;
}

} // namespace

std::string ws_accept_key(const std::string& client_key) {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] { ok = psa_crypto_init() == PSA_SUCCESS; });  // idempotent, shared with the TLS and checksum code
    if (!ok) throw std::runtime_error("cannot initialise the crypto backend");
    const std::string input = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[PSA_HASH_LENGTH(PSA_ALG_SHA_1)];
    std::size_t len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_1, reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest, sizeof digest,
                         &len) != PSA_SUCCESS)
        throw std::runtime_error("SHA-1 failed");
    return base64(digest, len);
}

bool ws_key_is_valid(const std::string& key) {
    if (key.size() != 24 || key[22] != '=' || key[23] != '=') return false;
    for (std::size_t i = 0; i < 22; ++i)
        if (!b64_char(key[i])) return false;
    return std::strchr("AQgw", key[21]) != nullptr;  // 16 bytes leave the last 4 bits of the 22nd character zero
}

bool ws_utf8_valid(std::string_view s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    auto cont = [&](std::size_t at, unsigned lo = 0x80, unsigned hi = 0xBF) { return at < n && p[at] >= lo && p[at] <= hi; };
    while (i < n) {
        const unsigned c = p[i];
        if (c < 0x80) { ++i; continue; }
        if (c >= 0xC2 && c <= 0xDF) { if (!cont(i + 1)) return false; i += 2; continue; }
        if (c >= 0xE0 && c <= 0xEF) {
            const unsigned lo = c == 0xE0 ? 0xA0 : 0x80, hi = c == 0xED ? 0x9F : 0xBF;  // no overlongs, no surrogates
            if (!cont(i + 1, lo, hi) || !cont(i + 2)) return false;
            i += 3;
            continue;
        }
        if (c >= 0xF0 && c <= 0xF4) {
            const unsigned lo = c == 0xF0 ? 0x90 : 0x80, hi = c == 0xF4 ? 0x8F : 0xBF;  // nothing above U+10FFFF
            if (!cont(i + 1, lo, hi) || !cont(i + 2) || !cont(i + 3)) return false;
            i += 4;
            continue;
        }
        return false;
    }
    return true;
}

bool ws_close_code_valid(unsigned c) {
    return (c >= 1000 && c <= 1003) || (c >= 1007 && c <= 1014) || (c >= 3000 && c <= 4999);
}

std::string ws_frame(unsigned opcode, std::string_view payload) {
    std::string f;
    f.reserve(payload.size() + 10);
    f.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() < 126) {
        f.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() < 65536) {
        f.push_back(126);
        f.push_back(static_cast<char>(payload.size() >> 8));
        f.push_back(static_cast<char>(payload.size() & 0xff));
    } else {
        f.push_back(127);
        for (int k = 7; k >= 0; --k) f.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> (8 * k)) & 0xff));
    }
    f.append(payload.data(), payload.size());
    return f;
}

// ---------------------------------------------------------------- the handle side

void ws_shared::run(std::function<void(ws_session&, tunnel_io&)> fn) {
    if (std::this_thread::get_id() == loop_thread_) {
        auto s = session_.lock();
        if (s && s->io_) fn(*s, *s->io_);
        return;
    }
    if (!post_) return;
    post_([w = session_, fn = std::move(fn)](tunnel_io& io) {
        if (auto s = w.lock()) fn(*s, io);
    });
}

ws_status ws_shared::send(unsigned opcode, std::string_view payload) {
    if (opcode == ws_ping && payload.size() > max_control_payload) throw std::invalid_argument("a ping holds at most 125 bytes");
    if (!is_open()) return ws_status::closed;
    if (buffered_.load(std::memory_order_relaxed) >= max_buffered_) {
        full_.store(true, std::memory_order_relaxed);
        return ws_status::full;
    }
    std::string frame = ws_frame(opcode, payload);
    buffered_.fetch_add(frame.size(), std::memory_order_relaxed);
    run([frame = std::move(frame)](ws_session& s, tunnel_io& io) mutable { s.enqueue(io, std::move(frame)); });
    return ws_status::ok;
}

void ws_shared::close(std::uint16_t code, std::string_view reason) {
    if (!is_open()) return;
    run([code, reason = std::string(reason)](ws_session& s, tunnel_io& io) { s.start_close(io, code, reason, true); });
}

void ws_shared::pause_reading(bool pause) {
    run([pause](ws_session&, tunnel_io& io) { io.pause_reading(pause); });
}

// ---------------------------------------------------------------- the session

std::shared_ptr<ws_session> ws_session::make(ws_events events, ws_limits limits) {
    auto s = std::make_shared<ws_session>(private_tag{}, std::move(events), limits);
    s->shared_->session_ = s;
    return s;
}

ws_session::ws_session(private_tag, ws_events events, ws_limits limits)
    : events_(std::move(events)), limits_(limits), shared_(std::make_shared<ws_shared>()) {
    shared_->max_buffered_ = limits.max_buffered;
}

void ws_session::on_open(tunnel_io& io, tunnel_post_fn post) {
    io_ = &io;
    opened_ = true;
    shared_->post_ = std::move(post);
    shared_->loop_thread_ = std::this_thread::get_id();
    shared_->open_.store(true, std::memory_order_release);
    if (limits_.ping_interval > 0) io.set_idle(limits_.ping_interval);
    if (events_.on_open) {
        try {
            events_.on_open();
        } catch (...) {
            HTTPP_LOG(warning) << "a websocket on_open handler threw: closing with 1011";
            fail(io, 1011);
        }
    }
}

void ws_session::enqueue(tunnel_io& io, std::string frame) {
    if (close_sent_ || dead_) {  // a send that raced a close: it was counted, so give the bytes back
        shared_->buffered_.fetch_sub(frame.size(), std::memory_order_relaxed);
        return;
    }
    io.write(std::move(frame));
}

void ws_session::start_close(tunnel_io& io, std::uint16_t code, std::string_view reason, bool wait_for_peer) {
    if (close_sent_) return;
    close_sent_ = true;
    shared_->open_.store(false, std::memory_order_release);
    std::string frame = ws_frame(ws_close, close_body(code, reason));
    shared_->buffered_.fetch_add(frame.size(), std::memory_order_relaxed);
    io.write(std::move(frame));
    if (wait_for_peer) {
        io.set_deadline(close_wait);
    } else {
        dead_ = true;
        io.close_after_flush();
    }
}

void ws_session::fail(tunnel_io& io, std::uint16_t code) {
    if (close_sent_) {  // already closing: just stop listening
        dead_ = true;
        io.close_after_flush();
        return;
    }
    start_close(io, code, {}, false);
}

void ws_session::on_written(tunnel_io&, std::size_t n) {
    const std::size_t was = shared_->buffered_.fetch_sub(std::min(n, shared_->buffered_.load(std::memory_order_relaxed)),
                                                         std::memory_order_relaxed);
    const std::size_t now = was - std::min(n, was);
    if (shared_->full_.load(std::memory_order_relaxed) && now <= limits_.max_buffered / 2) {
        shared_->full_.store(false, std::memory_order_relaxed);
        if (events_.on_drain && !close_sent_) {
            try {
                events_.on_drain();
            } catch (...) {
                HTTPP_LOG(warning) << "a websocket on_drain handler threw: ignored";
            }
        }
    }
}

void ws_session::on_idle(tunnel_io& io) {
    if (close_sent_ || dead_) return;
    if (ping_outstanding_) {  // a whole interval and not a byte since our ping
        HTTPP_LOG(debug) << "websocket peer did not answer a ping: closing";
        return start_close(io, 1001, {}, false);
    }
    ping_outstanding_ = true;
    std::string frame = ws_frame(ws_ping, {});
    shared_->buffered_.fetch_add(frame.size(), std::memory_order_relaxed);
    io.write(std::move(frame));
}

void ws_session::on_close() {
    if (closed_notified_) return;
    closed_notified_ = true;
    shared_->open_.store(false, std::memory_order_release);
    io_ = nullptr;
    if (opened_ && events_.on_close) {  // a connection that died before its 101 went out never opened
        try {
            events_.on_close(peer_code_, peer_reason_);
        } catch (...) {
            HTTPP_LOG(warning) << "a websocket on_close handler threw: ignored";
        }
    }
}

namespace {

// Unmasks `n` bytes (the key rotated by `offset`) from `src` onto the end of `dst`.
void append_unmasked(std::string& dst, const char* src, std::size_t n, const unsigned char key[4], std::uint64_t offset) {
    const std::size_t old = dst.size();
    dst.resize(old + n);
    char* d = &dst[old];
    unsigned char m[8];
    for (int i = 0; i < 8; ++i) m[i] = key[(offset + static_cast<unsigned>(i)) & 3];
    std::uint64_t mask;
    std::memcpy(&mask, m, 8);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t v;
        std::memcpy(&v, src + i, 8);
        v ^= mask;
        std::memcpy(d + i, &v, 8);
    }
    for (; i < n; ++i) d[i] = static_cast<char>(static_cast<unsigned char>(src[i]) ^ key[(offset + i) & 3]);
}

} // namespace

bool ws_session::parse_header(tunnel_io& io) {
    const unsigned b0 = head_[0], b1 = head_[1];
    if (b0 & 0x70) return fail(io, 1002), false;            // no extension is negotiated, so no RSV bit may be set
    fin_ = (b0 & 0x80) != 0;
    opcode_ = b0 & 0x0f;
    masked_ = (b1 & 0x80) != 0;
    if (!masked_) return fail(io, 1002), false;             // a client must mask
    const bool control = opcode_ >= 8;
    if (!(opcode_ == ws_continuation || opcode_ == ws_text || opcode_ == ws_binary || opcode_ == ws_close || opcode_ == ws_ping ||
          opcode_ == ws_pong))
        return fail(io, 1002), false;
    std::uint64_t len = b1 & 0x7f;
    std::size_t at = 2;
    if (len == 126) {
        len = (static_cast<std::uint64_t>(head_[2]) << 8) | head_[3];
        at = 4;
    } else if (len == 127) {
        len = 0;
        for (int k = 0; k < 8; ++k) len = (len << 8) | head_[2 + k];
        at = 10;
        if (len >> 63) return fail(io, 1002), false;
    }
    std::memcpy(mask_, head_ + at, 4);
    if (control) {
        if (!fin_ || len > max_control_payload) return fail(io, 1002), false;
        ctrl_.clear();
    } else {
        if (opcode_ == ws_continuation) {
            if (!in_message_) return fail(io, 1002), false;
        } else {
            if (in_message_) return fail(io, 1002), false;
            in_message_ = true;
            msg_binary_ = opcode_ == ws_binary;
            msg_.clear();
        }
        if (len > limits_.max_message || msg_.size() > limits_.max_message - len) return fail(io, 1009), false;  // before buffering a byte
        msg_.reserve(msg_.size() + static_cast<std::size_t>(std::min<std::uint64_t>(len, 1u << 20)));
    }
    remaining_ = len;
    offset_ = 0;
    return true;
}

void ws_session::on_control(tunnel_io& io) {
    if (opcode_ == ws_ping) {
        if (close_sent_) return;
        std::string frame = ws_frame(ws_pong, ctrl_);
        shared_->buffered_.fetch_add(frame.size(), std::memory_order_relaxed);
        io.write(std::move(frame));
        return;
    }
    if (opcode_ == ws_pong) return;
    // close
    std::uint16_t code = 1005;
    std::string reason;
    if (ctrl_.size() == 1) return fail(io, 1002);
    if (ctrl_.size() >= 2) {
        code = static_cast<std::uint16_t>((static_cast<unsigned char>(ctrl_[0]) << 8) | static_cast<unsigned char>(ctrl_[1]));
        if (!ws_close_code_valid(code)) return fail(io, 1002);
        reason = ctrl_.substr(2);
        if (!ws_utf8_valid(reason)) return fail(io, 1007);
    }
    got_close_ = true;
    peer_code_ = code;
    peer_reason_ = std::move(reason);
    if (close_sent_) {  // we closed first: this is the answer
        dead_ = true;
        io.close_after_flush();
    } else {
        start_close(io, code == 1005 ? 0 : code, peer_reason_, false);  // echo it
    }
}

void ws_session::finish_frame(tunnel_io& io) {
    stage_ = stage::header;
    head_have_ = 0;
    head_need_ = 2;
    if (opcode_ >= 8) return on_control(io);
    if (!fin_) return;
    in_message_ = false;
    if (!msg_binary_ && !ws_utf8_valid(msg_)) {
        msg_.clear();
        return fail(io, 1007);
    }
    if (!close_sent_ && events_.on_message) {
        try {
            events_.on_message(msg_, msg_binary_);
        } catch (...) {
            HTTPP_LOG(warning) << "a websocket on_message handler threw: closing with 1011";
            fail(io, 1011);
        }
    }
    if (msg_.capacity() > keep_capacity) std::string().swap(msg_);
    else msg_.clear();
}

void ws_session::on_bytes(tunnel_io& io, const char* data, std::size_t n) {
    ping_outstanding_ = false;
    while (n > 0 && !dead_) {
        if (stage_ == stage::header) {
            while (n > 0 && head_have_ < head_need_) {
                head_[head_have_++] = static_cast<unsigned char>(*data++);
                --n;
                if (head_have_ == 2) {
                    const unsigned len7 = head_[1] & 0x7f;
                    head_need_ = 2 + (len7 == 126 ? 2 : len7 == 127 ? 8 : 0) + ((head_[1] & 0x80) ? 4 : 0);
                }
            }
            if (head_have_ < head_need_) return;
            if (!parse_header(io)) return;
            if (remaining_ == 0) {
                finish_frame(io);
                continue;
            }
            stage_ = stage::payload;
        } else {
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(n, remaining_));
            append_unmasked(opcode_ >= 8 ? ctrl_ : msg_, data, take, mask_, offset_);
            offset_ += take;
            remaining_ -= take;
            data += take;
            n -= take;
            if (remaining_ == 0) finish_frame(io);
        }
    }
}

} // namespace httpp::detail
