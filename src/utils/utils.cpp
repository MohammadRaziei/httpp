#include "httpp/utils.hpp"

#include "internal/windows_text.hpp" // widen(), narrow()

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace httpp::env {

namespace {

// One lock for every call here, in this module only: libhttpp_utils is one shared library, so there is one lock per process.
std::mutex& environment_lock() {
    static std::mutex lock;
    return lock;
}

} // namespace

#ifdef _WIN32

// ---- Windows: the CRT's wide environment (getenv() reads the same copy that _wputenv_s() writes) ------------------

std::optional<std::string> get(const std::string& name) {
    const std::wstring wide_name = detail::widen(name);
    std::lock_guard<std::mutex> hold(environment_lock());
    std::size_t needed = 0; // with the terminating null; 0 when the variable is not set
    if (_wgetenv_s(&needed, nullptr, 0, wide_name.c_str()) != 0 || needed == 0) return std::nullopt;
    std::wstring value(needed, L'\0');
    if (_wgetenv_s(&needed, &value[0], value.size(), wide_name.c_str()) != 0 || needed == 0) return std::nullopt;
    value.resize(needed - 1);
    return detail::narrow(value);
}

bool set(const std::string& name, const std::string& value) {
    const std::wstring wide_name = detail::widen(name);
    const std::wstring wide_value = detail::widen(value);
    std::lock_guard<std::mutex> hold(environment_lock());
    return _wputenv_s(wide_name.c_str(), wide_value.c_str()) == 0; // an empty value removes the variable
}

bool unset(const std::string& name) { return set(name, std::string()); }

#else

// ---- POSIX -----------------------------------------------------------------------------------------------------

std::optional<std::string> get(const std::string& name) {
    std::lock_guard<std::mutex> hold(environment_lock());
    const char* value = std::getenv(name.c_str());
    if (!value) return std::nullopt;
    return std::string(value); // copied under the lock: a later set() may free what getenv() pointed to
}

bool set(const std::string& name, const std::string& value) {
    std::lock_guard<std::mutex> hold(environment_lock());
    return ::setenv(name.c_str(), value.c_str(), 1) == 0; // EINVAL for an empty name or one with '='
}

bool unset(const std::string& name) {
    std::lock_guard<std::mutex> hold(environment_lock());
    return ::unsetenv(name.c_str()) == 0;
}

#endif

// ---- the same everywhere ---------------------------------------------------------------------------------------

std::string first(std::initializer_list<const char*> names) {
    for (const char* name : names)
        if (auto value = get(name); value && !value->empty()) return std::move(*value);
    return std::string();
}

scope::scope(const std::string& name, const std::string& value) : name_(name), previous_(get(name)) { set(name_, value); }

scope::scope(const std::string& name) : name_(name), previous_(get(name)) { unset(name_); }

bool scope::restore() {
    if (!std::exchange(active_, false)) return false;
    return previous_ ? set(name_, *previous_) : unset(name_);
}

} // namespace httpp::env

// ---- SHA-256 (FIPS 180-4) ------------------------------------------------------------------------------------------------------

namespace httpp {

namespace {

constexpr std::uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

// The running hash: whole 64-byte blocks go through compress() as they arrive, the rest waits in `block`.
class sha256_state {
public:
    void update(const void* data, std::size_t size) {
        if (size == 0) return;
        const auto* bytes = static_cast<const unsigned char*>(data);
        total_bytes_ += size;
        if (block_size_ > 0) { // finish the block that is waiting
            const std::size_t take = std::min(size, sizeof block_ - block_size_);
            std::memcpy(block_ + block_size_, bytes, take);
            block_size_ += take;
            bytes += take;
            size -= take;
            if (block_size_ < sizeof block_) return;
            compress(block_);
            block_size_ = 0;
        }
        for (; size >= sizeof block_; bytes += sizeof block_, size -= sizeof block_) compress(bytes);
        std::memcpy(block_, bytes, size); // the tail waits for more, or for finish()
        block_size_ = size;
    }

    std::string finish() { // the 64 lowercase hex characters; the object is spent afterwards
        const std::uint64_t bits = total_bytes_ * 8;
        unsigned char tail[sizeof block_ * 2] = {0}; // the waiting bytes, 0x80, zeros, the length: one block or two
        std::memcpy(tail, block_, block_size_);
        tail[block_size_] = 0x80;
        const std::size_t used = block_size_ + 1 + 8 <= sizeof block_ ? sizeof block_ : sizeof tail;
        for (int i = 0; i < 8; ++i) tail[used - 1 - i] = static_cast<unsigned char>(bits >> (8 * i));
        for (std::size_t at = 0; at < used; at += sizeof block_) compress(tail + at);
        static const char* const hex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (std::uint32_t word : h_)
            for (int shift = 28; shift >= 0; shift -= 4) out += hex[(word >> shift) & 15];
        return out;
    }

private:
    void compress(const unsigned char* p) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) | (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + sha256_k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    std::uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char block_[64];
    std::size_t block_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

constexpr std::size_t max_checksum_file_bytes = 1024;

bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// The part of the text that is safe to put into an error message (an HTML page is not).
std::string preview(const std::string& text) {
    std::string out;
    for (char c : text.substr(0, 60)) out += std::isprint(static_cast<unsigned char>(c)) ? c : '.';
    return out;
}

} // namespace

std::string sha256_hex(const void* data, std::size_t size) {
    sha256_state h;
    h.update(data, size);
    return h.finish();
}

std::string sha256_file(const path& file) {
    std::ifstream in(file.native(), std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + file.str());
    sha256_state h;
    std::vector<char> buf(64 * 1024);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (in.gcount() > 0) h.update(buf.data(), static_cast<std::size_t>(in.gcount()));
    }
    if (!in.eof()) throw std::runtime_error("error while reading " + file.str());
    return h.finish();
}

std::string parse_sha256_checksum(const std::string& text, const std::string& file_name) {
    const std::string what = "not a valid checksum file (expected one line: <64 hex digits>[  <file name>]); starts with: '" +
                             preview(text) + "'";
    if (text.size() > max_checksum_file_bytes) throw std::runtime_error("Checksum file is too large to be one line. It is " + what);
    std::string line = text;
    if (!line.empty() && line.back() == '\n') line.pop_back();
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find('\n') != std::string::npos || line.find('\r') != std::string::npos) {
        throw std::runtime_error("Checksum file has more than one line. It is " + what);
    }
    if (line.size() < 64) throw std::runtime_error("Checksum file is " + what);
    std::string digest;
    for (std::size_t i = 0; i < 64; ++i) {
        if (!is_hex(line[i])) throw std::runtime_error("Checksum file is " + what);
        digest += static_cast<char>(std::tolower(static_cast<unsigned char>(line[i])));
    }
    if (line.size() > 64) {
        std::size_t pos = 64;
        if (line[pos] != ' ' && line[pos] != '\t') throw std::runtime_error("Checksum file is " + what);
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
        if (pos < line.size() && line[pos] == '*') ++pos;  // sha256sum's binary-mode marker
        const std::string name = line.substr(pos);
        if (name != file_name) throw std::runtime_error("Checksum file is for '" + name + "', not for '" + file_name + "'");
    }
    return digest;
}

} // namespace httpp

// ---- the end of a thread httpp started ------------------------------------------------------------------------------------------

namespace httpp {

namespace {
// One pointer per process (libhttpp_utils is one shared library). Set once, at import time of the Python binding, and read at every
// thread end, so an atomic with acquire/release is all it takes.
std::atomic<thread_exit_fn> g_thread_exit_hook{nullptr};
} // namespace

void set_thread_exit_hook(thread_exit_fn fn) noexcept { g_thread_exit_hook.store(fn, std::memory_order_release); }

void run_thread_exit_hook() noexcept {
    if (thread_exit_fn fn = g_thread_exit_hook.load(std::memory_order_acquire)) fn();
}

} // namespace httpp
