#pragma once
// A bare WebSocket client over a blocking-style Asio socket, for the server tests. It sends whatever bytes a test wants, so it can
// also be wrong on purpose (an unmasked frame, a bad handshake). Every read has a timeout.
#ifndef ASIO_STANDALONE
#  define ASIO_STANDALONE
#endif
#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <string>

namespace testutil {

struct ws_frame_in {
    unsigned op = 0;
    bool fin = false;
    std::string payload;
};

inline std::string ws_mask_frame(unsigned op, const std::string& payload, bool fin = true, bool mask = true) {
    std::string f;
    f.push_back(static_cast<char>((fin ? 0x80 : 0) | op));
    const unsigned char key[4] = {0x12, 0x34, 0x56, 0x78};
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

inline std::string ws_close_body(unsigned code, const std::string& reason = "") {
    std::string p;
    p.push_back(static_cast<char>(code >> 8));
    p.push_back(static_cast<char>(code & 0xff));
    return p + reason;
}

struct ws_client {
    using ms = std::chrono::milliseconds;
    asio::io_context io;
    asio::ip::tcp::socket sock{io};
    std::string rest;      // bytes read and not consumed yet
    bool eof = false;

    explicit ws_client(int port) {
        sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), static_cast<unsigned short>(port)));
        sock.set_option(asio::ip::tcp::no_delay(true));
    }

    void send_raw(const std::string& bytes) { asio::write(sock, asio::buffer(bytes)); }
    void send_frame(unsigned op, const std::string& payload, bool fin = true) { send_raw(ws_mask_frame(op, payload, fin)); }

    static std::string upgrade_request(const std::string& path, const std::string& extra = "",
                                       const std::string& key = "dGhlIHNhbXBsZSBub25jZQ==", const std::string& version = "13") {
        std::string r = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
        if (!key.empty()) r += "Sec-WebSocket-Key: " + key + "\r\n";
        if (!version.empty()) r += "Sec-WebSocket-Version: " + version + "\r\n";
        return r + extra + "\r\n";
    }

    bool read_more(ms timeout) {
        char buf[65536];
        std::error_code ec;
        std::size_t n = 0;
        bool done = false;
        sock.async_read_some(asio::buffer(buf), [&](std::error_code e, std::size_t k) { ec = e; n = k; done = true; });
        io.restart();
        io.run_for(timeout);
        if (!done) {
            sock.cancel();
            io.restart();
            io.run();
            return false;
        }
        if (ec) { eof = true; return false; }
        rest.append(buf, n);
        return true;
    }
    bool fill(std::size_t n, ms timeout = ms(3000)) {
        while (rest.size() < n) if (!read_more(timeout)) return false;
        return true;
    }

    // The response head, up to and including the blank line; "" on timeout or EOF.
    std::string read_head(ms timeout = ms(3000)) {
        for (;;) {
            const std::size_t end = rest.find("\r\n\r\n");
            if (end != std::string::npos) {
                std::string head = rest.substr(0, end + 4);
                rest.erase(0, end + 4);
                return head;
            }
            if (!read_more(timeout)) return "";
        }
    }
    std::string handshake(const std::string& path, const std::string& extra = "") {
        send_raw(upgrade_request(path, extra));
        return read_head();
    }

    bool read_frame(ws_frame_in& f, ms timeout = ms(3000)) {
        if (!fill(2, timeout)) return false;
        const unsigned char b0 = static_cast<unsigned char>(rest[0]), b1 = static_cast<unsigned char>(rest[1]);
        std::uint64_t len = b1 & 0x7f;
        std::size_t at = 2;
        if (len == 126) { if (!fill(4, timeout)) return false; len = (static_cast<unsigned char>(rest[2]) << 8) | static_cast<unsigned char>(rest[3]); at = 4; }
        else if (len == 127) {
            if (!fill(10, timeout)) return false;
            len = 0;
            for (int k = 0; k < 8; ++k) len = (len << 8) | static_cast<unsigned char>(rest[2 + k]);
            at = 10;
        }
        if (!fill(at + static_cast<std::size_t>(len), timeout)) return false;
        f.op = b0 & 0x0f;
        f.fin = (b0 & 0x80) != 0;
        f.payload = rest.substr(at, static_cast<std::size_t>(len));
        rest.erase(0, at + static_cast<std::size_t>(len));
        return true;
    }

    // True when the server closed the connection (everything still in flight is discarded) within the timeout.
    bool closed_by_peer(ms timeout = ms(3000)) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!eof) {
            const auto left = std::chrono::duration_cast<ms>(end - std::chrono::steady_clock::now());
            if (left.count() <= 0) return false;
            read_more(left);
            rest.clear();
        }
        return true;
    }
};

inline int head_status(const std::string& head) {
    return head.size() > 12 ? std::atoi(head.c_str() + 9) : 0;
}

// First header value, "" if absent (names are compared case-insensitively).
inline std::string head_header(const std::string& head, std::string name) {
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::size_t pos = head.find("\r\n");
    while (pos != std::string::npos && pos + 2 < head.size()) {
        pos += 2;
        const std::size_t end = head.find("\r\n", pos);
        const std::string line = head.substr(pos, end - pos);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (key == name) {
                std::size_t v = colon + 1;
                while (v < line.size() && line[v] == ' ') ++v;
                return line.substr(v);
            }
        }
        pos = end;
    }
    return "";
}

} // namespace testutil
