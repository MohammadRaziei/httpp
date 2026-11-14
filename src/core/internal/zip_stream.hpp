#pragma once

// Internal. A ZIP archive produced piece by piece, for the file browser's "download as ZIP" (ADR-0032).
//
// Nothing is compressed: every file goes in as deflate "stored" blocks (method 8, 5 bytes of framing per 64 KB). A stored block
// is a legal deflate stream, so the entry ends by itself and a data descriptor can carry the CRC-32 and the sizes after the data:
// one pass over each file, no second read to find the CRC, and extractors that read the archive from the front (not only from the
// central directory) still work. Names are UTF-8 (flag bit 11). An entry of 4 GiB or more, an archive that passes 4 GiB, or more than
// 65534 entries use Zip64 (APPNOTE 4.5.3, 4.3.14 and 4.3.15).
//
// The sizes are taken from the plan: exactly `size` bytes are read from each file. A file that is shorter by then makes next() throw
// (the connection is dropped, as for a file that ends early); a file that grew is cut at the planned size.

#include "httpp/path.hpp"
#include "checksums.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace httpp::detail {

struct zip_item {
    std::string name;             // inside the archive: UTF-8, '/' between parts, a folder ends with '/'; the caller has checked it
    httpp::path file;             // on disk (unused for a folder)
    unsigned long long size = 0;  // when the plan was made
    long long mtime = 0;          // seconds since 1970
    bool dir = false;
};

class zip_stream {
public:
    // `zip64_from`: the size from which a file is written with 64-bit fields. `big_from`: the offset (and size of the central
    // directory) from which the 64-bit forms are used there. Not public settings: tests lower them to reach those paths without 4 GiB.
    explicit zip_stream(std::vector<zip_item> items, unsigned long long zip64_from = 0xFFC00000ull, unsigned long long big_from = 0xFFFFFFFFull)
        : items_(std::move(items)), zip64_from_(zip64_from), big_from_(big_from) {
        for (const zip_item& it : items_)
            if (it.name.empty() || it.name.size() > 0xFFFF) throw std::invalid_argument("zip: bad entry name");
    }

    // Appends the next piece of the archive to `piece` (cleared first). Returns false when the archive is complete; the last piece
    // can be non-empty. Throws std::runtime_error when a file cannot be read.
    bool next(std::string& piece) {
        piece.clear();
        while (phase_ != phase::done && piece.size() < k_piece) step(piece);
        emitted_ += piece.size();
        return phase_ != phase::done;
    }

private:
    static constexpr std::size_t k_piece = 64 * 1024;
    static constexpr std::size_t k_block = 65535;  // the most a stored deflate block holds

    struct record {
        std::string name;
        std::uint32_t crc = 0;
        unsigned long long csize = 0, usize = 0, offset = 0;
        std::uint16_t dos_date = 0, dos_time = 0;
        bool dir = false, zip64 = false;
    };
    enum class phase { header, data, tail, central, finish, done };

    std::vector<zip_item> items_;
    unsigned long long zip64_from_, big_from_;
    phase phase_ = phase::header;
    std::size_t idx_ = 0;              // the entry being written
    unsigned long long emitted_ = 0;   // bytes in the pieces already returned
    std::vector<record> done_;         // the entries written, for the central directory
    record rec_;                       // the entry being written
    std::fstream in_;
    std::uint32_t crc_ = 0;
    unsigned long long left_ = 0, csize_ = 0;
    std::string buf_;
    std::size_t cd_idx_ = 0;
    unsigned long long cd_start_ = 0, cd_size_ = 0;

    unsigned long long pos(const std::string& out) const { return emitted_ + out.size(); }

    static void put16(std::string& o, std::uint32_t v) { o += static_cast<char>(v & 0xFF); o += static_cast<char>((v >> 8) & 0xFF); }
    static void put32(std::string& o, std::uint32_t v) { put16(o, v & 0xFFFF); put16(o, (v >> 16) & 0xFFFF); }
    static void put64(std::string& o, unsigned long long v) { put32(o, static_cast<std::uint32_t>(v & 0xFFFFFFFFull)); put32(o, static_cast<std::uint32_t>(v >> 32)); }

    static void dos_stamp(long long t, std::uint16_t& date, std::uint16_t& time) {
        std::time_t tt = static_cast<std::time_t>(t);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        int year = tm.tm_year + 1900;
        if (year < 1980) { date = static_cast<std::uint16_t>((1 << 5) | 1); time = 0; return; }  // 1980-01-01, the earliest a DOS date can say
        if (year > 2107) year = 2107;
        date = static_cast<std::uint16_t>(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
        time = static_cast<std::uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    }

    void step(std::string& out) {
        switch (phase_) {
            case phase::header:
                if (idx_ == items_.size()) { phase_ = phase::central; cd_start_ = pos(out); cd_idx_ = 0; }
                else begin_entry(out);
                break;
            case phase::data: data_block(out); break;
            case phase::tail: end_entry(out); break;
            case phase::central: central_entry(out); break;
            case phase::finish: finish(out); phase_ = phase::done; break;
            case phase::done: break;
        }
    }

    void begin_entry(std::string& out) {
        const zip_item& it = items_[idx_];
        rec_ = record{};
        rec_.name = it.name;
        rec_.dir = it.dir;
        rec_.usize = it.dir ? 0 : it.size;
        rec_.zip64 = !it.dir && it.size >= zip64_from_;
        rec_.offset = pos(out);
        dos_stamp(it.mtime, rec_.dos_date, rec_.dos_time);

        put32(out, 0x04034b50u);
        put16(out, rec_.zip64 ? 45 : 20);                    // version needed
        put16(out, it.dir ? 0x0800 : 0x0808);                // UTF-8 names; for a file, the sizes follow the data
        put16(out, it.dir ? 0 : 8);                          // method: stored for a folder, deflate (with stored blocks) for a file
        put16(out, rec_.dos_time);
        put16(out, rec_.dos_date);
        put32(out, 0);                                       // crc: in the descriptor
        put32(out, rec_.zip64 ? 0xFFFFFFFFu : 0);            // sizes: in the descriptor (0xFFFFFFFF says "look in the Zip64 field")
        put32(out, rec_.zip64 ? 0xFFFFFFFFu : 0);
        put16(out, static_cast<std::uint32_t>(it.name.size()));
        put16(out, rec_.zip64 ? 20 : 0);
        out += it.name;
        if (rec_.zip64) { put16(out, 1); put16(out, 16); put64(out, 0); put64(out, 0); }

        if (it.dir) { done_.push_back(rec_); ++idx_; return; }  // a folder has no data and no descriptor
        in_.close();
        in_.clear();
        in_ = it.file.open(std::ios::in | std::ios::binary);
        if (!in_) throw std::runtime_error("zip: cannot open a file");
        crc_ = 0;
        csize_ = 0;
        left_ = it.size;
        phase_ = left_ > 0 ? phase::data : phase::tail;
    }

    void data_block(std::string& out) {
        const std::size_t n = static_cast<std::size_t>(std::min<unsigned long long>(left_, k_block));
        buf_.resize(n);
        in_.read(&buf_[0], static_cast<std::streamsize>(n));
        if (static_cast<std::size_t>(in_.gcount()) != n) throw std::runtime_error("zip: a file ended early");  // the connection is dropped
        crc_ = crc32_update(crc_, reinterpret_cast<const unsigned char*>(buf_.data()), n);
        out += '\0';                                         // BFINAL 0, BTYPE 00 (stored)
        put16(out, static_cast<std::uint32_t>(n));
        put16(out, static_cast<std::uint32_t>(~n) & 0xFFFF);
        out += buf_;
        csize_ += 5 + n;
        left_ -= n;
        if (left_ == 0) phase_ = phase::tail;
    }

    void end_entry(std::string& out) {
        out += '\x01';                                       // the last block: empty, BFINAL 1
        put16(out, 0);
        put16(out, 0xFFFF);
        csize_ += 5;
        rec_.crc = crc_;
        rec_.csize = csize_;
        put32(out, 0x08074b50u);                             // the data descriptor
        put32(out, rec_.crc);
        if (rec_.zip64) { put64(out, rec_.csize); put64(out, rec_.usize); }
        else { put32(out, static_cast<std::uint32_t>(rec_.csize)); put32(out, static_cast<std::uint32_t>(rec_.usize)); }
        done_.push_back(rec_);
        in_.close();
        ++idx_;
        phase_ = phase::header;
    }

    void central_entry(std::string& out) {
        if (cd_idx_ == done_.size()) { cd_size_ = pos(out) - cd_start_; phase_ = phase::finish; return; }
        const record& r = done_[cd_idx_++];
        const bool big_off = r.offset >= big_from_;
        const std::size_t fields = (r.zip64 ? 2 : 0) + (big_off ? 1 : 0);
        put32(out, 0x02014b50u);
        put16(out, (3 << 8) | 45);                           // made by: Unix, 4.5
        put16(out, (r.zip64 || big_off) ? 45 : 20);
        put16(out, r.dir ? 0x0800 : 0x0808);
        put16(out, r.dir ? 0 : 8);
        put16(out, r.dos_time);
        put16(out, r.dos_date);
        put32(out, r.crc);
        put32(out, r.zip64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(r.csize));
        put32(out, r.zip64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(r.usize));
        put16(out, static_cast<std::uint32_t>(r.name.size()));
        put16(out, fields ? static_cast<std::uint32_t>(4 + 8 * fields) : 0);
        put16(out, 0);                                       // comment
        put16(out, 0);                                       // disk
        put16(out, 0);                                       // internal attributes
        put32(out, r.dir ? ((040755u << 16) | 0x10u) : (0100644u << 16));  // Unix mode in the high half, the DOS "folder" bit
        put32(out, big_off ? 0xFFFFFFFFu : static_cast<std::uint32_t>(r.offset));
        out += r.name;
        if (fields) {
            put16(out, 1);
            put16(out, static_cast<std::uint32_t>(8 * fields));
            if (r.zip64) { put64(out, r.usize); put64(out, r.csize); }
            if (big_off) put64(out, r.offset);
        }
    }

    void finish(std::string& out) {
        const unsigned long long n = done_.size();
        const bool big = n >= 0xFFFF || cd_size_ >= big_from_ || cd_start_ >= big_from_;
        if (big) {
            const unsigned long long at = pos(out);
            put32(out, 0x06064b50u);                         // Zip64 end of central directory record
            put64(out, 44);
            put16(out, (3 << 8) | 45);
            put16(out, 45);
            put32(out, 0);
            put32(out, 0);
            put64(out, n);
            put64(out, n);
            put64(out, cd_size_);
            put64(out, cd_start_);
            put32(out, 0x07064b50u);                         // its locator
            put32(out, 0);
            put64(out, at);
            put32(out, 1);
        }
        put32(out, 0x06054b50u);                             // end of central directory
        put16(out, 0);
        put16(out, 0);
        put16(out, big ? 0xFFFF : static_cast<std::uint32_t>(n));
        put16(out, big ? 0xFFFF : static_cast<std::uint32_t>(n));
        put32(out, big ? 0xFFFFFFFFu : static_cast<std::uint32_t>(cd_size_));
        put32(out, big ? 0xFFFFFFFFu : static_cast<std::uint32_t>(cd_start_));
        put16(out, 0);
    }
};

}  // namespace httpp::detail
