// The WebSocket codec and session (src/core/internal/websocket.*), driven with a fake tunnel_io: no sockets.
#include "utest/utest.h"
#include "internal/websocket.hpp"

#include <chrono>
#include <string>
#include <vector>

using namespace httpp::detail;

namespace {

struct fake_io : tunnel_io {
    std::string written;           // every byte handed to write(), in order
    std::size_t pending = 0;       // not yet reported through on_written
    bool closed = false, after_flush = false, paused = false;
    int idle = -1;
    std::chrono::milliseconds deadline{-1};
    void write(std::string data) override { written += data; pending += data.size(); }
    std::size_t queued() const override { return pending; }
    void pause_reading(bool p) override { paused = p; }
    void close() override { closed = true; }
    void close_after_flush() override { after_flush = true; }
    void set_idle(int s) override { idle = s; }
    void set_deadline(std::chrono::milliseconds d) override { deadline = d; }
};

struct frame {
    unsigned op;
    bool fin;
    std::string payload;
};

// What the server wrote, as frames (server frames are never masked).
std::vector<frame> frames_of(const std::string& bytes) {
    std::vector<frame> out;
    std::size_t i = 0;
    while (i + 2 <= bytes.size()) {
        const unsigned char b0 = static_cast<unsigned char>(bytes[i]), b1 = static_cast<unsigned char>(bytes[i + 1]);
        std::uint64_t len = b1 & 0x7f;
        std::size_t at = i + 2;
        if (len == 126) { len = (static_cast<unsigned char>(bytes[at]) << 8) | static_cast<unsigned char>(bytes[at + 1]); at += 2; }
        else if (len == 127) { len = 0; for (int k = 0; k < 8; ++k) len = (len << 8) | static_cast<unsigned char>(bytes[at + k]); at += 8; }
        out.push_back({b0 & 0x0fu, (b0 & 0x80) != 0, bytes.substr(at, static_cast<std::size_t>(len))});
        i = at + static_cast<std::size_t>(len);
    }
    return out;
}

// A client frame (always masked).
std::string client_frame(unsigned op, const std::string& payload, bool fin = true, unsigned char rsv = 0, bool mask = true) {
    std::string f;
    f.push_back(static_cast<char>((fin ? 0x80 : 0) | rsv | op));
    const unsigned char key[4] = {0x37, 0xfa, 0x21, 0x3d};
    const unsigned char m = mask ? 0x80 : 0;
    if (payload.size() < 126) {
        f.push_back(static_cast<char>(m | payload.size()));
    } else if (payload.size() < 65536) {
        f.push_back(static_cast<char>(m | 126));
        f.push_back(static_cast<char>(payload.size() >> 8));
        f.push_back(static_cast<char>(payload.size() & 0xff));
    } else {
        f.push_back(static_cast<char>(m | 127));
        for (int k = 7; k >= 0; --k) f.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> (8 * k)) & 0xff));
    }
    if (mask) f.append(reinterpret_cast<const char*>(key), 4);
    for (std::size_t i = 0; i < payload.size(); ++i) f.push_back(static_cast<char>(payload[i] ^ (mask ? key[i % 4] : 0)));
    return f;
}

std::string close_payload(unsigned code, const std::string& reason = "") {
    std::string p;
    p.push_back(static_cast<char>(code >> 8));
    p.push_back(static_cast<char>(code & 0xff));
    return p + reason;
}

struct message {
    std::string data;
    bool binary;
};

struct rig {
    fake_io io;
    std::vector<message> messages;
    int opened = 0, drained = 0, closes = 0;
    unsigned close_code = 0;
    std::string close_reason;
    std::shared_ptr<ws_session> session;
    std::shared_ptr<ws_shared> shared;

    explicit rig(ws_limits lim = {}, std::function<void(rig&, std::string_view, bool)> on_msg = {}) {
        ws_events ev;
        ev.on_open = [this] { ++opened; };
        ev.on_message = [this, on_msg](std::string_view m, bool bin) {
            messages.push_back({std::string(m), bin});
            if (on_msg) on_msg(*this, m, bin);
        };
        ev.on_drain = [this] { ++drained; };
        ev.on_close = [this](std::uint16_t code, std::string_view reason) {
            ++closes;
            close_code = code;
            close_reason = std::string(reason);
        };
        session = ws_session::make(std::move(ev), lim);
        shared = session->shared();
        session->on_open(io, [this](std::function<void(tunnel_io&)> fn) { fn(io); });
    }
    void feed(const std::string& bytes) { session->on_bytes(io, bytes.data(), bytes.size()); }
    // Reports everything written so far as written.
    void flush() {
        const std::size_t n = io.pending;
        io.pending = 0;
        session->on_written(io, n);
    }
    std::vector<frame> out() const { return frames_of(io.written); }
    // The last frame written must be a close with this code.
    bool closed_with(unsigned code) const {
        const auto fs = out();
        return !fs.empty() && fs.back().op == ws_close && fs.back().payload.size() >= 2 &&
               ((static_cast<unsigned char>(fs.back().payload[0]) << 8) | static_cast<unsigned char>(fs.back().payload[1])) == code;
    }
};

} // namespace

// ---------------------------------------------------------------- codec

UTEST(httpp_ws_codec, the_accept_key_of_the_rfc_example) {
    ASSERT_TRUE(ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

UTEST(httpp_ws_codec, a_key_is_valid_only_as_base64_of_16_bytes) {
    ASSERT_TRUE(ws_key_is_valid("dGhlIHNhbXBsZSBub25jZQ=="));
    ASSERT_FALSE(ws_key_is_valid(""));
    ASSERT_FALSE(ws_key_is_valid("dGhlIHNhbXBsZSBub25jZQ="));      // 23 characters
    ASSERT_FALSE(ws_key_is_valid("dGhlIHNhbXBsZSBub25jZQ=A"));     // padding in the wrong place
    ASSERT_FALSE(ws_key_is_valid("dGhlIHNhbXBsZSBub25j!Q=="));     // not base64
    ASSERT_FALSE(ws_key_is_valid("dGhlIHNhbXBsZSBub25jZQ==\r\n")); // trailing bytes
    ASSERT_FALSE(ws_key_is_valid("dGhlIHNhbXBsZSBub25jZXhy"));     // 18 bytes
}

UTEST(httpp_ws_codec, utf8_validation) {
    ASSERT_TRUE(ws_utf8_valid(""));
    ASSERT_TRUE(ws_utf8_valid("plain ascii"));
    ASSERT_TRUE(ws_utf8_valid("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"));
    ASSERT_TRUE(ws_utf8_valid("\xF4\x8F\xBF\xBF"));                 // U+10FFFF, the last one
    ASSERT_FALSE(ws_utf8_valid("\xC3"));                            // truncated
    ASSERT_FALSE(ws_utf8_valid("\x80"));                            // a lone continuation byte
    ASSERT_FALSE(ws_utf8_valid("\xC0\xAF"));                        // overlong "/"
    ASSERT_FALSE(ws_utf8_valid("\xE0\x80\xAF"));                    // overlong
    ASSERT_FALSE(ws_utf8_valid("\xED\xA0\x80"));                    // a surrogate
    ASSERT_FALSE(ws_utf8_valid("\xF4\x90\x80\x80"));                // above U+10FFFF
    ASSERT_FALSE(ws_utf8_valid("\xFF"));
}

UTEST(httpp_ws_codec, close_codes) {
    for (unsigned c : {1000u, 1001u, 1002u, 1003u, 1007u, 1008u, 1009u, 1010u, 1011u, 1012u, 1013u, 1014u, 3000u, 4999u})
        ASSERT_TRUE(ws_close_code_valid(c));
    for (unsigned c : {0u, 999u, 1004u, 1005u, 1006u, 1015u, 1016u, 2999u, 5000u, 65535u})
        ASSERT_FALSE(ws_close_code_valid(c));
}

UTEST(httpp_ws_codec, a_frame_has_the_shortest_length_form) {
    ASSERT_TRUE(ws_frame(ws_text, "hi") == std::string("\x81\x02hi", 4));
    const std::string p125(125, 'a'), p126(126, 'a'), p65535(65535, 'a'), p65536(65536, 'a');
    ASSERT_EQ(2u + 125u, ws_frame(ws_binary, p125).size());
    ASSERT_EQ(4u + 126u, ws_frame(ws_binary, p126).size());
    ASSERT_EQ(4u + 65535u, ws_frame(ws_binary, p65535).size());
    ASSERT_EQ(10u + 65536u, ws_frame(ws_binary, p65536).size());
    const std::string f = ws_frame(ws_binary, p126);
    ASSERT_EQ(0x82, static_cast<unsigned char>(f[0]));
    ASSERT_EQ(126, static_cast<unsigned char>(f[1]));
    ASSERT_EQ(0, static_cast<unsigned char>(f[2]));
    ASSERT_EQ(126, static_cast<unsigned char>(f[3]));
    ASSERT_EQ(0x89, static_cast<unsigned char>(ws_frame(ws_ping, "")[0]));
}

// ---------------------------------------------------------------- receiving

UTEST(httpp_ws_session, opening_calls_on_open_and_nothing_is_written) {
    rig r;
    ASSERT_EQ(1, r.opened);
    ASSERT_TRUE(r.shared->is_open());
    ASSERT_TRUE(r.io.written.empty());
}

UTEST(httpp_ws_session, a_text_and_a_binary_message_arrive_with_their_kind) {
    rig r;
    r.feed(client_frame(ws_text, "hello") + client_frame(ws_binary, std::string("\x00\x01\xff", 3)));
    ASSERT_EQ(2u, r.messages.size());
    ASSERT_TRUE(r.messages[0].data == "hello" && !r.messages[0].binary);
    ASSERT_TRUE(r.messages[1].data == std::string("\x00\x01\xff", 3) && r.messages[1].binary);
}

UTEST(httpp_ws_session, bytes_may_arrive_one_at_a_time) {
    rig r;
    const std::string wire = client_frame(ws_text, std::string(300, 'x')) + client_frame(ws_text, "second");
    for (char c : wire) r.feed(std::string(1, c));
    ASSERT_EQ(2u, r.messages.size());
    ASSERT_EQ(300u, r.messages[0].data.size());
    ASSERT_TRUE(r.messages[1].data == "second");
}

UTEST(httpp_ws_session, a_64_bit_length_frame_is_read) {
    rig r({1u << 20, 1u << 20, 0});
    r.feed(client_frame(ws_binary, std::string(70000, 'q')));
    ASSERT_EQ(1u, r.messages.size());
    ASSERT_EQ(70000u, r.messages[0].data.size());
}

UTEST(httpp_ws_session, fragments_are_joined_and_a_ping_may_come_between_them) {
    rig r;
    r.feed(client_frame(ws_text, "Hel", false));
    r.feed(client_frame(ws_ping, "p"));
    r.feed(client_frame(ws_continuation, "lo, ", false));
    r.feed(client_frame(ws_continuation, "world", true));
    ASSERT_EQ(1u, r.messages.size());
    ASSERT_TRUE(r.messages[0].data == "Hello, world" && !r.messages[0].binary);
    const auto fs = r.out();
    ASSERT_EQ(1u, fs.size());
    ASSERT_EQ(static_cast<unsigned>(ws_pong), fs[0].op);
    ASSERT_TRUE(fs[0].payload == "p");
}

UTEST(httpp_ws_session, a_ping_is_answered_with_a_pong_of_the_same_payload) {
    rig r;
    r.feed(client_frame(ws_ping, "are you there"));
    const auto fs = r.out();
    ASSERT_EQ(1u, fs.size());
    ASSERT_EQ(static_cast<unsigned>(ws_pong), fs[0].op);
    ASSERT_TRUE(fs[0].payload == "are you there");
    ASSERT_TRUE(r.messages.empty());
}

UTEST(httpp_ws_session, an_unsolicited_pong_is_ignored) {
    rig r;
    r.feed(client_frame(ws_pong, "x"));
    ASSERT_TRUE(r.io.written.empty());
    ASSERT_FALSE(r.io.after_flush);
}

UTEST(httpp_ws_session, an_empty_message_is_a_message) {
    rig r;
    r.feed(client_frame(ws_text, ""));
    ASSERT_EQ(1u, r.messages.size());
    ASSERT_TRUE(r.messages[0].data.empty());
}

UTEST(httpp_ws_session, multi_byte_text_split_over_fragments_is_valid) {
    rig r;
    const std::string euro = "\xE2\x82\xAC";
    r.feed(client_frame(ws_text, euro.substr(0, 1), false));
    r.feed(client_frame(ws_continuation, euro.substr(1), true));
    ASSERT_EQ(1u, r.messages.size());
    ASSERT_TRUE(r.messages[0].data == euro);
}

// ---------------------------------------------------------------- protocol errors

UTEST(httpp_ws_session, an_unmasked_client_frame_is_a_protocol_error) {
    rig r;
    r.feed(client_frame(ws_text, "hi", true, 0, false));
    ASSERT_TRUE(r.closed_with(1002));
    ASSERT_TRUE(r.io.after_flush);
    ASSERT_TRUE(r.messages.empty());
}

UTEST(httpp_ws_session, reserved_bits_are_a_protocol_error) {
    for (unsigned char rsv : {0x40, 0x20, 0x10}) {
        rig r;
        r.feed(client_frame(ws_text, "hi", true, rsv));
        ASSERT_TRUE(r.closed_with(1002));
    }
}

UTEST(httpp_ws_session, reserved_opcodes_are_a_protocol_error) {
    for (unsigned op : {3u, 4u, 5u, 6u, 7u, 11u, 12u, 13u, 14u, 15u}) {
        rig r;
        r.feed(client_frame(op, "x"));
        ASSERT_TRUE(r.closed_with(1002));
    }
}

UTEST(httpp_ws_session, control_frames_must_be_whole_and_short) {
    rig fragmented;
    fragmented.feed(client_frame(ws_ping, "x", false));
    ASSERT_TRUE(fragmented.closed_with(1002));
    rig big;
    big.feed(client_frame(ws_ping, std::string(126, 'x')));
    ASSERT_TRUE(big.closed_with(1002));
    rig edge;
    edge.feed(client_frame(ws_ping, std::string(125, 'x')));
    ASSERT_EQ(static_cast<unsigned>(ws_pong), edge.out().back().op);
}

UTEST(httpp_ws_session, fragment_order_is_checked) {
    rig stray;
    stray.feed(client_frame(ws_continuation, "x"));
    ASSERT_TRUE(stray.closed_with(1002));
    rig nested;
    nested.feed(client_frame(ws_text, "a", false));
    nested.feed(client_frame(ws_text, "b"));
    ASSERT_TRUE(nested.closed_with(1002));
    ASSERT_TRUE(nested.messages.empty());
}

UTEST(httpp_ws_session, a_64_bit_length_with_the_top_bit_set_is_a_protocol_error) {
    rig r;
    std::string f = "\x82\xFF";
    f += std::string("\x80\x00\x00\x00\x00\x00\x00\x00", 8);
    f += std::string(4, '\0');
    r.feed(f);
    ASSERT_TRUE(r.closed_with(1002));
}

UTEST(httpp_ws_session, a_message_over_the_limit_is_refused_before_it_is_buffered) {
    rig r({100, 1u << 20, 0});
    // only the header of a 1 MiB frame; no payload follows
    std::string f = "\x82\xFE";
    f.push_back(static_cast<char>(0x10));
    f.push_back(static_cast<char>(0x00));
    f += std::string("\x01\x02\x03\x04", 4);
    r.feed(f);
    ASSERT_TRUE(r.closed_with(1009));
    ASSERT_TRUE(r.messages.empty());
}

UTEST(httpp_ws_session, the_limit_counts_every_fragment_of_a_message) {
    rig r({100, 1u << 20, 0});
    r.feed(client_frame(ws_binary, std::string(60, 'a'), false));
    r.feed(client_frame(ws_continuation, std::string(60, 'a'), true));
    ASSERT_TRUE(r.closed_with(1009));
    ASSERT_TRUE(r.messages.empty());
    rig exact({100, 1u << 20, 0});
    exact.feed(client_frame(ws_binary, std::string(100, 'a')));
    ASSERT_EQ(1u, exact.messages.size());
}

UTEST(httpp_ws_session, text_that_is_not_utf8_is_refused_with_1007) {
    rig r;
    r.feed(client_frame(ws_text, "ok \xC3\x28"));
    ASSERT_TRUE(r.closed_with(1007));
    ASSERT_TRUE(r.messages.empty());
    rig bin;  // binary is not checked
    bin.feed(client_frame(ws_binary, "\xC3\x28"));
    ASSERT_EQ(1u, bin.messages.size());
}

UTEST(httpp_ws_session, bytes_after_a_protocol_error_are_ignored) {
    rig r;
    r.feed(client_frame(ws_text, "x", true, 0x40) + client_frame(ws_text, "late"));
    ASSERT_TRUE(r.messages.empty());
    ASSERT_EQ(1u, r.out().size());
}

UTEST(httpp_ws_session, a_handler_that_throws_closes_with_1011) {
    rig r({}, [](rig&, std::string_view, bool) { throw 1; });
    r.feed(client_frame(ws_text, "boom"));
    ASSERT_TRUE(r.closed_with(1011));
}

// ---------------------------------------------------------------- closing

UTEST(httpp_ws_session, a_close_from_the_peer_is_echoed_and_the_connection_ends_after_the_flush) {
    rig r;
    r.feed(client_frame(ws_close, close_payload(1000, "bye")));
    const auto fs = r.out();
    ASSERT_EQ(1u, fs.size());
    ASSERT_EQ(static_cast<unsigned>(ws_close), fs[0].op);
    ASSERT_TRUE(fs[0].payload == close_payload(1000, "bye"));
    ASSERT_TRUE(r.io.after_flush);
    ASSERT_FALSE(r.shared->is_open());
    r.session->on_close();
    ASSERT_EQ(1, r.closes);
    ASSERT_EQ(1000u, r.close_code);
    ASSERT_TRUE(r.close_reason == "bye");
}

UTEST(httpp_ws_session, an_empty_close_is_echoed_empty_and_reported_as_1005) {
    rig r;
    r.feed(client_frame(ws_close, ""));
    const auto fs = r.out();
    ASSERT_EQ(1u, fs.size());
    ASSERT_TRUE(fs[0].payload.empty());
    r.session->on_close();
    ASSERT_EQ(1005u, r.close_code);
}

UTEST(httpp_ws_session, a_bad_close_payload_is_a_protocol_error) {
    rig one;
    one.feed(client_frame(ws_close, "x"));
    ASSERT_TRUE(one.closed_with(1002));
    for (unsigned bad : {0u, 999u, 1004u, 1005u, 1006u, 1015u, 2000u, 5000u}) {
        rig r;
        r.feed(client_frame(ws_close, close_payload(bad)));
        ASSERT_TRUE(r.closed_with(1002));
    }
    rig reason;
    reason.feed(client_frame(ws_close, close_payload(1000, "\xC3\x28")));
    ASSERT_TRUE(reason.closed_with(1007));
}

UTEST(httpp_ws_session, a_close_by_us_waits_for_the_peer_and_has_a_deadline) {
    rig r;
    r.shared->close(1001, "going away");
    const auto fs = r.out();
    ASSERT_EQ(1u, fs.size());
    ASSERT_TRUE(fs[0].payload == close_payload(1001, "going away"));
    ASSERT_FALSE(r.shared->is_open());
    ASSERT_FALSE(r.io.after_flush);
    ASSERT_EQ(5000, static_cast<int>(r.io.deadline.count()));
    r.feed(client_frame(ws_close, close_payload(1000)));
    ASSERT_EQ(1u, r.out().size()); // no second close frame
    ASSERT_TRUE(r.io.after_flush);
    r.session->on_close();
    ASSERT_EQ(1000u, r.close_code);
}

UTEST(httpp_ws_session, closing_twice_sends_one_frame) {
    rig r;
    r.shared->close(1000, "");
    r.shared->close(1001, "");
    ASSERT_EQ(1u, r.out().size());
}

UTEST(httpp_ws_session, a_connection_that_ends_without_a_close_frame_is_1006) {
    rig r;
    r.session->on_close();
    ASSERT_EQ(1, r.closes);
    ASSERT_EQ(1006u, r.close_code);
    ASSERT_FALSE(r.shared->is_open());
    r.session->on_close();  // never twice
    ASSERT_EQ(1, r.closes);
}

UTEST(httpp_ws_session, a_close_from_us_without_an_answer_is_reported_as_1006) {
    rig r;
    r.shared->close(1000, "");
    r.session->on_close();
    ASSERT_EQ(1006u, r.close_code);
}

// ---------------------------------------------------------------- sending

UTEST(httpp_ws_session, a_handler_may_answer_from_inside_on_message) {
    rig r({}, [](rig& self, std::string_view m, bool) { self.shared->send(ws_text, "echo:" + std::string(m)); });
    r.feed(client_frame(ws_text, "abc"));
    const auto fs = r.out();
    ASSERT_EQ(1u, fs.size());
    ASSERT_TRUE(fs[0].fin);
    ASSERT_EQ(static_cast<unsigned>(ws_text), fs[0].op);
    ASSERT_TRUE(fs[0].payload == "echo:abc");
}

UTEST(httpp_ws_session, sends_are_written_in_order_and_counted_until_written) {
    rig r;
    ASSERT_TRUE(r.shared->send(ws_text, "one") == ws_status::ok);
    ASSERT_TRUE(r.shared->send(ws_binary, "two") == ws_status::ok);
    ASSERT_TRUE(r.shared->send(ws_ping, "") == ws_status::ok);
    const auto fs = r.out();
    ASSERT_EQ(3u, fs.size());
    ASSERT_TRUE(fs[0].payload == "one" && fs[1].payload == "two");
    ASSERT_EQ(static_cast<unsigned>(ws_ping), fs[2].op);
    ASSERT_EQ(r.io.written.size(), r.shared->buffered());
    r.flush();
    ASSERT_EQ(0u, r.shared->buffered());
}

UTEST(httpp_ws_session, a_send_over_the_buffer_limit_is_refused_and_on_drain_tells_when_there_is_room) {
    rig r({1u << 20, 100, 0});
    ASSERT_TRUE(r.shared->send(ws_binary, std::string(60, 'a')) == ws_status::ok);   // 62 bytes
    ASSERT_TRUE(r.shared->send(ws_binary, std::string(60, 'b')) == ws_status::ok);   // 124: now over the limit
    ASSERT_TRUE(r.shared->send(ws_binary, std::string(10, 'c')) == ws_status::full);
    ASSERT_EQ(2u, r.out().size());                                                   // the refused one was not queued
    ASSERT_EQ(0, r.drained);
    r.session->on_written(r.io, 62);                                                 // 62 left: not yet below half
    ASSERT_EQ(0, r.drained);
    r.session->on_written(r.io, 62);                                                 // empty
    ASSERT_EQ(1, r.drained);
    r.session->on_written(r.io, 0);
    ASSERT_EQ(1, r.drained);                                                         // once per refusal
    ASSERT_TRUE(r.shared->send(ws_binary, "d") == ws_status::ok);
}

UTEST(httpp_ws_session, nothing_is_sent_once_closing_or_closed) {
    rig r;
    r.shared->close(1000, "");
    ASSERT_TRUE(r.shared->send(ws_text, "late") == ws_status::closed);
    ASSERT_EQ(1u, r.out().size());
    rig gone;
    gone.session->on_close();
    ASSERT_TRUE(gone.shared->send(ws_text, "late") == ws_status::closed);
}

UTEST(httpp_ws_session, a_handle_outlives_the_session) {
    std::shared_ptr<ws_shared> handle;
    {
        rig r;
        handle = r.shared;
        r.session->on_close();
    }
    ASSERT_FALSE(handle->is_open());
    ASSERT_TRUE(handle->send(ws_text, "x") == ws_status::closed);
    handle->close(1000, "");   // does nothing, does not crash
}

UTEST(httpp_ws_session, pausing_reading_reaches_the_connection) {
    rig r;
    r.shared->pause_reading(true);
    ASSERT_TRUE(r.io.paused);
    r.shared->pause_reading(false);
    ASSERT_FALSE(r.io.paused);
}

// ---------------------------------------------------------------- keep-alive pings

UTEST(httpp_ws_session, the_idle_interval_is_asked_for_only_when_set) {
    rig off;
    ASSERT_EQ(0, off.io.idle == -1 ? 0 : off.io.idle);
    rig on({1u << 20, 1u << 20, 7});
    ASSERT_EQ(7, on.io.idle);
}

UTEST(httpp_ws_session, a_quiet_peer_gets_a_ping_and_then_is_closed) {
    rig r({1u << 20, 1u << 20, 5});
    r.session->on_idle(r.io);
    auto fs = r.out();
    ASSERT_EQ(1u, fs.size());
    ASSERT_EQ(static_cast<unsigned>(ws_ping), fs[0].op);
    r.session->on_idle(r.io);                       // nothing came back
    ASSERT_TRUE(r.closed_with(1001));
    ASSERT_TRUE(r.io.after_flush);
}

UTEST(httpp_ws_session, any_bytes_from_the_peer_clear_the_outstanding_ping) {
    rig r({1u << 20, 1u << 20, 5});
    r.session->on_idle(r.io);
    r.feed(client_frame(ws_pong, ""));
    r.session->on_idle(r.io);                       // a new ping, not a close
    const auto fs = r.out();
    ASSERT_EQ(2u, fs.size());
    ASSERT_EQ(static_cast<unsigned>(ws_ping), fs[1].op);
    ASSERT_FALSE(r.io.after_flush);
}

UTEST(httpp_ws_session, a_session_that_never_opened_still_reports_its_close_once) {
    int closes = 0;
    std::uint16_t code = 0;
    {
        ws_events ev;
        ev.on_close = [&](std::uint16_t c, std::string_view) { ++closes; code = c; };
        auto session = ws_session::make(std::move(ev), {});
        session->on_close();  // the connection died before the 101 was written
        ASSERT_EQ(1, closes);
        ASSERT_EQ(1006u, code);
        ASSERT_FALSE(session->shared()->is_open());
    }
    ASSERT_EQ(1, closes);     // the destructor does not say it again
    int dropped = 0;
    {
        ws_events ev;
        ev.on_close = [&](std::uint16_t, std::string_view) { ++dropped; };
        auto session = ws_session::make(std::move(ev), {});  // dropped without the engine ever touching it
    }
    ASSERT_EQ(1, dropped);
}

UTEST(httpp_ws_session, the_user_pointer_lives_with_the_handle) {
    auto user = std::make_shared<int>(7);
    std::weak_ptr<int> weak = user;
    std::shared_ptr<ws_shared> handle;
    {
        auto session = ws_session::make({}, {}, user);
        handle = session->shared();
    }
    user.reset();
    ASSERT_FALSE(weak.expired());           // the handle still holds it
    ASSERT_EQ(7, *static_cast<int*>(handle->user().get()));
    handle.reset();
    ASSERT_TRUE(weak.expired());
}
