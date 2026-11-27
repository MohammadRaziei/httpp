#include "internal/body_source.hpp"

#include "httpp/server.hpp" // mime_type_for

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <random>
#include <stdexcept>

namespace httpp::detail {
namespace {

// A file that is read from the start for `size` bytes. A file shorter than that was changed while the request was being made.
class file_reader final : public body_reader {
public:
    file_reader(path file, std::uint64_t size) : file_(std::move(file)), name_(file_.str()), left_(size) {}

    bool open() {
        in_ = file_.open(std::ios::in | std::ios::binary);
        return static_cast<bool>(in_);
    }

    std::size_t read(char* out, std::size_t capacity) override {
        if (left_ == 0) return 0;
        const std::size_t want = left_ < capacity ? static_cast<std::size_t>(left_) : capacity;
        in_.read(out, static_cast<std::streamsize>(want));
        const std::size_t got = static_cast<std::size_t>(in_.gcount());
        if (got < want) throw std::runtime_error("'" + name_ + "' is shorter than it was when the request started");
        left_ -= got;
        return got;
    }

private:
    path file_;
    std::fstream in_;
    std::string name_;
    std::uint64_t left_;
};

// A multipart body is a list of pieces: text that is already built, and files that are read on demand.
struct segment {
    std::string text;       // is_file == false: the bytes
    path file;              // is_file == true: the path
    std::uint64_t size = 0; // is_file == true: the bytes to read
    bool is_file = false;
};

class multipart_reader final : public body_reader {
public:
    explicit multipart_reader(std::shared_ptr<const std::vector<segment>> segments) : segments_(std::move(segments)) {}

    std::size_t read(char* out, std::size_t capacity) override {
        std::size_t done = 0;
        while (done < capacity && index_ < segments_->size()) {
            const segment& s = (*segments_)[index_];
            if (!s.is_file) {
                const std::size_t take = std::min(capacity - done, s.text.size() - offset_);
                std::copy_n(s.text.data() + offset_, take, out + done);
                offset_ += take;
                done += take;
                if (offset_ == s.text.size()) next();
            } else {
                if (!current_) {
                    auto f = std::make_unique<file_reader>(s.file, s.size);
                    if (!f->open()) throw std::runtime_error("cannot read '" + s.file.str() + "'");
                    current_ = std::move(f);
                }
                const std::size_t got = current_->read(out + done, capacity - done);
                done += got;
                if (got == 0) next(); // the file is finished (read() returns 0 only at its end)
            }
        }
        return done;
    }

private:
    void next() {
        ++index_;
        offset_ = 0;
        current_.reset();
    }

    std::shared_ptr<const std::vector<segment>> segments_;
    std::size_t index_ = 0;
    std::size_t offset_ = 0;
    std::unique_ptr<file_reader> current_;
};

std::string random_boundary() {
    static const char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
    std::string out = "----httpp";
    for (int i = 0; i < 24; ++i) out += hex[gen() & 15];
    return out;
}

// A value inside a quoted parameter of Content-Disposition: a quote is escaped (the server side reads \" back), a line break cannot be sent.
std::string escape_quoted(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '"') out += "\\\"";
        else if (c == '\r' || c == '\n') out += ' ';
        else out += c;
    }
    return out;
}

std::string base_name(const std::string& file) {
    const auto slash = file.find_last_of("/\\");
    return slash == std::string::npos ? file : file.substr(slash + 1);
}

} // namespace

std::shared_ptr<body_source> make_file_source(const path& file, std::string& error) {
    const long long size = file.size();
    if (size < 0) {
        error = "cannot read '" + file.str() + "': not a file";
        return nullptr;
    }
    if (!file_reader(file, 0).open()) {
        error = "cannot read '" + file.str() + "'";
        return nullptr;
    }
    auto source = std::make_shared<body_source>();
    source->length = static_cast<std::uint64_t>(size);
    const std::uint64_t length = source->length;
    source->open = [file, length](std::string& why) -> std::unique_ptr<body_reader> {
        auto reader = std::make_unique<file_reader>(file, length);
        if (!reader->open()) {
            why = "cannot read '" + file.str() + "'";
            return nullptr;
        }
        return reader;
    };
    return source;
}

std::shared_ptr<body_source> make_multipart_source(const std::vector<form_part>& parts, std::string& content_type, std::string& error) {
    const std::string boundary = random_boundary(); // ponytail: not checked against the content; 96 random bits make a clash not worth a scan of every file. Upgrade: scan text parts, or retry with a new boundary
    auto segments = std::make_shared<std::vector<segment>>();
    std::uint64_t length = 0;
    auto add_text = [&](std::string text) {
        length += text.size();
        if (!segments->empty() && !segments->back().is_file) {
            segments->back().text += text;
        } else {
            segment s;
            s.text = std::move(text);
            segments->push_back(std::move(s));
        }
    };
    for (const form_part& p : parts) {
        if (p.name.empty()) {
            error = "a form part needs a name";
            return nullptr;
        }
        std::string head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + escape_quoted(p.name) + "\"";
        if (p.what == form_part::kind::text) {
            head += "\r\n\r\n";
            add_text(std::move(head));
            add_text(p.value);
            add_text("\r\n");
            continue;
        }
        std::string filename = p.filename;
        if (filename.empty() && p.what == form_part::kind::file) filename = base_name(p.value);
        std::string type = p.content_type;
        if (type.empty()) type = filename.empty() ? "application/octet-stream" : mime_type_for(filename);
        head += "; filename=\"" + escape_quoted(filename) + "\"\r\nContent-Type: " + type + "\r\n\r\n";
        add_text(std::move(head));
        if (p.what == form_part::kind::data) {
            add_text(p.value);
        } else {
            const path file(p.value);
            const long long size = file.size();
            if (size < 0) {
                error = "cannot read '" + p.value + "': not a file";
                return nullptr;
            }
            if (!file_reader(file, 0).open()) {
                error = "cannot read '" + p.value + "'";
                return nullptr;
            }
            segment s;
            s.is_file = true;
            s.file = file;
            s.size = static_cast<std::uint64_t>(size);
            length += s.size;
            segments->push_back(std::move(s));
        }
        add_text("\r\n");
    }
    add_text("--" + boundary + "--\r\n");

    content_type = "multipart/form-data; boundary=" + boundary;
    auto source = std::make_shared<body_source>();
    source->length = length;
    std::shared_ptr<const std::vector<segment>> shared = segments;
    source->open = [shared](std::string&) -> std::unique_ptr<body_reader> { return std::make_unique<multipart_reader>(shared); };
    return source;
}

} // namespace httpp::detail
