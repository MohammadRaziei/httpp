#include "httpp/sha256.hpp"

#include <psa/crypto.h>

#include <cctype>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace httpp {

namespace {

constexpr std::size_t max_checksum_file_bytes = 1024;

void init_crypto() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] { ok = psa_crypto_init() == PSA_SUCCESS; });  // idempotent, shared with the TLS code
    if (!ok) throw std::runtime_error("cannot initialise the crypto backend");
}

struct hash_op {
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    hash_op() {
        init_crypto();
        if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) throw std::runtime_error("cannot start SHA-256");
    }
    ~hash_op() { psa_hash_abort(&op); }
    hash_op(const hash_op&) = delete;
    hash_op& operator=(const hash_op&) = delete;

    void update(const void* data, std::size_t size) {
        if (psa_hash_update(&op, static_cast<const uint8_t*>(data), size) != PSA_SUCCESS) {
            throw std::runtime_error("SHA-256 failed");
        }
    }
    std::string finish() {
        uint8_t digest[PSA_HASH_LENGTH(PSA_ALG_SHA_256)];
        std::size_t len = 0;
        if (psa_hash_finish(&op, digest, sizeof digest, &len) != PSA_SUCCESS) throw std::runtime_error("SHA-256 failed");
        static const char* const hex = "0123456789abcdef";
        std::string out;
        for (std::size_t i = 0; i < len; ++i) {
            out += hex[digest[i] >> 4];
            out += hex[digest[i] & 15];
        }
        return out;
    }
};

bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// The part of the text that is safe to put into an error message (an HTML page is not).
std::string preview(const std::string& text) {
    std::string out;
    for (char c : text.substr(0, 60)) out += std::isprint(static_cast<unsigned char>(c)) ? c : '.';
    return out;
}

} // namespace

std::string sha256_hex(const void* data, std::size_t size) {
    hash_op h;
    h.update(data, size);
    return h.finish();
}

std::string sha256_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    hash_op h;
    std::vector<char> buf(64 * 1024);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        if (in.gcount() > 0) h.update(buf.data(), static_cast<std::size_t>(in.gcount()));
    }
    if (!in.eof()) throw std::runtime_error("error while reading " + path);
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
