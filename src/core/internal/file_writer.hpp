#pragma once

// The file a body is written to, shared by request::save_to() (ADR-0102) and download. It does the mechanics and says what went
// wrong in words that name the path; what to write, when to open and what an answer is worth stay with the caller.

#include "httpp/client.hpp"
#include "httpp/path.hpp"

#include <cstddef>
#include <fstream>
#include <string>

namespace httpp::detail {

class file_writer {
public:
    enum class mode { truncate, append };

    // Opens `file`, closing any file this writer had open. create_dirs: make the folder above it first. replace = false: a
    // file that is already there is an error (and is not touched). Returns false and sets error() on failure.
    bool open(const path& file, mode how, bool create_dirs, bool replace) {
        close();
        error_.clear();
        shown_ = file.str();
        if (!replace && file.exists()) {
            error_ = "Refusing to overwrite '" + shown_ + "': File exists";
            return false;
        }
        if (create_dirs && !file.parent().mkdirs()) {
            error_ = "cannot create the folder of '" + shown_ + "'";
            return false;
        }
        out_.open(file.native(), std::ios::binary | (how == mode::append ? std::ios::app : std::ios::trunc));
        if (!out_) {
            error_ = "cannot write '" + shown_ + "'";
            return false;
        }
        return true;
    }

    bool write(const char* data, std::size_t size) {
        out_.write(data, static_cast<std::streamsize>(size));
        if (!out_) {
            error_ = "cannot write '" + shown_ + "'";
            return false;
        }
        return true;
    }

    void close() {
        if (out_.is_open()) out_.close();
        out_.clear(); // a failed earlier open must not make the next one fail
    }

    bool is_open() const { return out_.is_open(); }
    const std::string& error() const { return error_; }

private:
    std::ofstream out_;
    std::string shown_;
    std::string error_;
};

// The status line and headers as curl -i prints them: "HTTP/1.1 200", one "Name: value" per header, an empty line, all CRLF.
inline std::string head_text(const response& res) {
    std::string out = "HTTP/1.1 " + std::to_string(res.status) + "\r\n";
    for (const auto& [name, value] : res.headers) out += name + ": " + value + "\r\n";
    out += "\r\n";
    return out;
}

} // namespace httpp::detail
