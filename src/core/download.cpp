#include "httpp/download.hpp"
#include "httpp/url.hpp"
#include "httpp/progress.hpp"
#include "httpp/sha256.hpp"

#include "internal/fs.hpp"
#include "internal/http_exchange.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#include <process.h> // _getpid
#else
#include <unistd.h>  // getpid
#endif

namespace httpp {

namespace {

// The size of a regular file, or -1 (missing, a directory, ...).
long long file_size_or_negative(const std::string& path) {
    const fs::info i = fs::stat(path);
    return i.type == fs::kind::file ? i.size : -1;
}

// ---- one transfer: a request whose body streams into a file ----------------------------------------------------
// A download is the shared client policy (internal/http_exchange: redirects, timeouts, TLS) with a body_sink that
// writes the file, on the engine reserved for streaming transfers. The sink decides what a response is worth:
// the file is opened only for a good status, so an error page never lands in it.

struct transfer_result {
    bool ok = false;
    int status = 0;            // of the final response, 0 if none arrived
    bool real_content = false; // body bytes (200/206) reached the file: a failure may leave a usable .part
    std::string error;
};

class file_target {
public:
    // strict: only 200 and 206 count as success (a resumable download); otherwise any 2xx.
    // resume_offset > 0: the request asked for bytes from there; a 206 appends, any other success restarts the file.
    file_target(std::string path, bool strict, long long resume_offset, long long total_hint, bool show_progress)
        : path_(std::move(path)), strict_(strict), offset_(resume_offset), total_hint_(total_hint), progress_(show_progress) {}

    std::shared_ptr<detail::body_sink> sink() {
        auto s = std::make_shared<detail::body_sink>();
        s->on_head = [this](const response& head) { return on_head(head); };
        s->on_data = [this](const char* data, std::size_t size) { return on_data(data, size); };
        return s;
    }

    bool accepts(int status) const { return strict_ ? (status == 200 || status == 206) : (status >= 200 && status < 300); }
    int status() const { return status_; }
    bool real_content() const { return real_; }
    const std::string& error() const { return error_; } // set when this side stopped the transfer

    void finish() {
        out_.close();
        if (bar_) bar_->finish();
    }

private:
    detail::body_sink::verdict on_head(const response& head) {
        status_ = head.status;
        if (!accepts(status_)) return detail::body_sink::verdict::skip; // an error page, or a redirect we do not follow
        if (status_ != 206) offset_ = 0;                                 // the server ignored the Range: start over
        out_.open(fs::native(path_), std::ios::binary | (offset_ > 0 ? std::ios::app : std::ios::trunc));
        if (!out_) {
            error_ = "could not open destination file: " + path_;
            return detail::body_sink::verdict::abort;
        }
        real_ = true;
        if (progress_) { // the bar counts the whole file, also when this response is only the rest of it
            const std::string length = head.header("Content-Length");
            const long long total = total_hint_ > 0 ? total_hint_ : offset_ + (length.empty() ? 0 : std::atoll(length.c_str()));
            if (total > 0) bar_ = std::make_unique<progress::bar>(static_cast<std::size_t>(total), "downloading");
        }
        received_ = static_cast<std::size_t>(offset_);
        return detail::body_sink::verdict::stream;
    }

    bool on_data(const char* data, std::size_t size) {
        out_.write(data, static_cast<std::streamsize>(size));
        if (!out_) {
            error_ = "could not write to " + path_;
            return false;
        }
        received_ += size;
        if (bar_) bar_->set_progress(received_);
        return true;
    }

    std::string path_;
    bool strict_;
    long long offset_;
    long long total_hint_;
    bool progress_;
    std::ofstream out_;
    std::unique_ptr<progress::bar> bar_;
    std::size_t received_ = 0;
    int status_ = 0;
    bool real_ = false;
    std::string error_;
};

// The request every download step makes: identity coding (the bytes on disk are what the server sent, and a
// checksum is over those), redirects as asked, and `extra` headers.
bool make_transfer_spec(const std::string& url_str, const char* method, bool follow, long timeout_seconds,
                        const detail::header_list& extra, detail::request_spec& spec, std::string& error) {
    detail::session_settings settings;
    settings.follow = follow;
    settings.set_timeout(timeout_seconds);
    detail::header_list headers{{"Accept-Encoding", "identity"}};
    headers.insert(headers.end(), extra.begin(), extra.end());
    response bad;
    if (settings.make_spec_for_url(url_str, "", method, headers, {}, {}, spec, bad)) return true;
    error = "invalid or unsupported URL: " + url_str;
    return false;
}

transfer_result transfer(const std::string& url_str, bool follow, long timeout_seconds, long long range_from, file_target& target) {
    transfer_result result;
    detail::header_list extra;
    if (range_from > 0) extra.emplace_back("Range", "bytes=" + std::to_string(range_from) + "-");
    detail::request_spec spec;
    if (!make_transfer_spec(url_str, "GET", follow, timeout_seconds, extra, spec, result.error)) return result;
    spec.sink = target.sink();
    const response r = detail::run_blocking(std::make_shared<detail::request_spec>(std::move(spec)));
    target.finish();

    result.status = target.status() > 0 ? target.status() : r.status;
    result.real_content = target.real_content();
    if (!target.error().empty()) result.error = target.error();
    else if (r.error != error_kind::none) result.error = "request failed (connection error): " + r.error_message;
    else if (!target.accepts(r.status)) result.error = "request failed with status " + std::to_string(r.status);
    result.ok = result.error.empty();
    return result;
}

// The size the server reports for the whole file (HEAD), or -1 if it does not say.
long long probe_size(const std::string& url_str, bool follow, long timeout_seconds) {
    detail::request_spec spec;
    std::string error;
    if (!make_transfer_spec(url_str, "HEAD", follow, timeout_seconds, {}, spec, error)) return -1;
    const response r = detail::run_blocking(std::make_shared<detail::request_spec>(std::move(spec)));
    const std::string length = r.header("Content-Length");
    return r.status >= 200 && r.status < 300 && !length.empty() ? std::atoll(length.c_str()) : -1;
}

download_result to_result(const transfer_result& t) {
    download_result r;
    r.ok = t.ok;
    r.status = t.status;
    r.error = t.error;
    return r;
}

// Always-fresh implementation (no HEAD probe, no resume, no skip): the server's full body streams straight to
// dest_path. Used directly when resume() is off, for the checksum file, and as the fallback from run_resumable()
// when the server does not report a size.
download_result run_plain(const std::string& url_str, const std::string& dest_path, bool show_progress,
                          bool follow_redirects, long timeout_seconds) {
    file_target target(dest_path, false, 0, -1, show_progress);
    const transfer_result t = transfer(url_str, follow_redirects, timeout_seconds, 0, target);
    // A failed download must not leave a 0-byte (or half-written) file at dest_path: callers reasonably treat
    // "the file exists" as "it worked".
    if (!t.ok) fs::remove_file(dest_path);
    return to_result(t);
}

// resume()/force()-aware implementation: skip-if-complete, stage into dest_path + ".part", and Range-resume that
// .part if it already has bytes from an earlier attempt. See download::resume() in the header for the contract.
download_result run_resumable(const std::string& url_str, const std::string& dest_path, bool show_progress,
                              bool follow_redirects, bool force, long timeout_seconds) {
    download_result result;
    if (!url::parse(url_str).valid()) {
        result.error = "invalid or unsupported URL: " + url_str;
        return result;
    }

    // Find out the full size up front: both "already complete, skip" and "is this .part already the full file"
    // are checked against it. Without it resume/skip have nothing to compare with: do a plain download.
    const long long total_size = probe_size(url_str, follow_redirects, timeout_seconds);
    if (total_size < 0) return run_plain(url_str, dest_path, show_progress, follow_redirects, timeout_seconds);

    if (!force && file_size_or_negative(dest_path) == total_size) {
        result.ok = true;
        result.skipped = true;
        result.status = 200;
        return result;
    }

    const std::string part_path = dest_path + ".part";
    long long offset = force ? -1 : file_size_or_negative(part_path);
    if (offset < 0) {
        offset = 0;
    } else if (offset >= total_size) {
        // .part already holds the full file (a previous run finished writing but was interrupted before the
        // rename); nothing is left to transfer.
        if (fs::replace(part_path, dest_path)) {
            result.ok = true;
            result.status = 200;
            return result;
        }
        offset = 0; // rename failed; restart cleanly instead of erroring out
    }

    file_target target(part_path, true, offset, total_size, show_progress);
    const transfer_result t = transfer(url_str, follow_redirects, timeout_seconds, offset, target);
    result = to_result(t);
    if (t.ok) {
        result.ok = fs::replace(part_path, dest_path);
        if (!result.ok) result.error = "downloaded but could not rename " + part_path + " to " + dest_path;
    } else if (!t.real_content) {
        fs::remove_file(part_path); // never got real content (an error page, no connection): .part is not valid
    }
    return result;
}

// The checksum file is one short line; anything longer is read only as far as parse_sha256_checksum needs to reject it.
std::string read_prefix(const std::string& path, std::size_t limit) {
    std::ifstream in(fs::native(path), std::ios::binary);
    std::string text(limit, '\0');
    in.read(&text[0], static_cast<std::streamsize>(limit));
    text.resize(static_cast<std::size_t>(in.gcount()));
    return text;
}

// ---- the cross-process lock (download::lock) -------------------------------------------------------------------
// Creating a directory is atomic everywhere, so whoever creates "<dest>.lock" owns it (fs::make_dir). Liveness is
// judged by the waiter's own clock from whether `alive` *changes*, not from file times, so clock skew and coarse mtimes
// do not matter.

long current_pid() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

class dir_lock {
public:
    // `stale_after`: how long `alive` may stay unchanged before its owner is taken for dead (the download's timeout).
    dir_lock(std::string path, std::chrono::milliseconds stale_after) : path_(std::move(path)) {
        using clock = std::chrono::steady_clock;
        // the owner proves it lives twice per wait window (at most every 200 ms); waiters look more often than that
        // at most 200 ms: a waiter's threshold is whole seconds (>= 1 s), so it always sees several beats
        heartbeat_ = clamp(stale_after / 2, std::chrono::milliseconds(200));
        const auto poll = clamp(stale_after / 15, std::chrono::milliseconds(200));
        std::string last_seen;
        auto last_change = clock::now();
        bool first = true;
        for (;;) {
            const fs::made made = fs::make_dir(path_);
            if (made == fs::made::created) {
                owned_ = true;
                break;
            }
            if (made == fs::made::failed) break; // cannot create it (read-only directory?): go on without a lock
            waited_ = true;
            const std::string seen = read_prefix(fs::join(path_, "alive"), 64);
            const auto now = clock::now();
            if (first || seen != last_seen) { // the owner is alive (or this is the first look)
                last_seen = seen;
                last_change = now;
                first = false;
            } else if (now - last_change > stale_after) { // no sign of life for the whole timeout: its owner died
                fs::remove_file(fs::join(path_, "alive"));
                fs::remove_dir(path_);
                first = true;
                continue;
            }
            // ponytail: a lock that keeps being updated is waited for without limit; upgrade path: a cap
            std::this_thread::sleep_for(poll);
        }
        if (owned_) {
            beat(0);
            heart_ = std::thread([this] { run_heartbeat(); });
        }
    }

    ~dir_lock() {
        if (!owned_) return; // never touch a lock that belongs to someone else
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (heart_.joinable()) heart_.join();
        fs::remove_file(fs::join(path_, "alive"));
        fs::remove_dir(path_);
    }

    dir_lock(const dir_lock&) = delete;
    dir_lock& operator=(const dir_lock&) = delete;

    // true if someone else held the lock at some point, i.e. the file may have been downloaded meanwhile
    bool waited() const { return waited_; }

private:
    void beat(unsigned long long n) const {
        std::ofstream out(fs::native(fs::join(path_, "alive")), std::ios::binary | std::ios::trunc);
        out << current_pid() << ' ' << n << '\n';
    }

    void run_heartbeat() {
        std::unique_lock<std::mutex> lk(mutex_);
        unsigned long long n = 0;
        while (!stop_) {
            beat(++n);
            wake_.wait_for(lk, heartbeat_, [this] { return stop_; });
        }
    }

    static std::chrono::milliseconds clamp(std::chrono::milliseconds v, std::chrono::milliseconds most) {
        return v < std::chrono::milliseconds(5) ? std::chrono::milliseconds(5) : (v > most ? most : v);
    }

    std::string path_;
    std::chrono::milliseconds heartbeat_{1000};
    bool owned_ = false;
    bool waited_ = false;
    bool stop_ = false;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread heart_;
};

// Everything one download needs, copied out of the builder so run_async() can outlive it.
struct job {
    std::string url, dest;
    bool progress = true, follow = true, resume = false, force = false;
    std::string sha256, sha256_url;
    bool use_sha256_url = false;
    long timeout_seconds = 0; // 0: the default (300 s) for the network, 15 s for the lock
    std::chrono::milliseconds max_wait{2000}; // how long an unchanged lock is waited for
};

download_result failure(std::string error) {
    download_result r;
    r.error = std::move(error);
    return r;
}

std::string without_query(const std::string& url_str) {
    return url_str.substr(0, url_str.find_first_of("?#"));
}

std::string file_name_of(const std::string& url_str) {
    const std::string base = without_query(url_str);
    return base.substr(base.find_last_of('/') + 1);
}

// The digest the file must have, lowercase; empty if no verification was asked for. On a problem returns false.
bool expected_digest(const job& j, std::string& digest, std::string& error) {
    digest.clear();
    if (j.use_sha256_url) {
        const std::string sum_url = j.sha256_url.empty() ? without_query(j.url) + ".sha256" : j.sha256_url;
        const std::string tmp = j.dest + ".sha256.part";
        const download_result got = run_plain(sum_url, tmp, false, j.follow, j.timeout_seconds);
        if (!got.ok) {
            fs::remove_file(tmp);
            error = "could not download the checksum file " + sum_url + ": " + got.error;
            return false;
        }
        const std::string text = read_prefix(tmp, 1025);
        fs::remove_file(tmp);
        try {
            digest = parse_sha256_checksum(text, file_name_of(j.url));
        } catch (const std::exception& e) {
            error = std::string("bad checksum file ") + sum_url + ": " + e.what();
            return false;
        }
        return true;
    }
    if (j.sha256.empty()) return true;
    bool good = j.sha256.size() == 64;
    for (char c : j.sha256) good = good && std::isxdigit(static_cast<unsigned char>(c)) != 0;
    if (!good) {
        error = "invalid SHA-256 (expected 64 hex digits): " + j.sha256.substr(0, 80);
        return false;
    }
    for (char c : j.sha256) digest += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return true;
}

download_result execute(const job& j) {
    // Locked first: the checksum file is fetched to a temporary name next to dest, which two processes must not share.
    const long timeout = j.timeout_seconds > 0 ? j.timeout_seconds : 15;
    const auto stale_after = std::min(std::chrono::milliseconds(timeout * 1000), j.max_wait);
    const auto lock = std::make_unique<dir_lock>(j.dest + ".lock", stale_after);

    std::string expected, error;
    if (!expected_digest(j, expected, error)) return failure(error);  // before the file, so nothing is downloaded

    if (lock->waited() && !expected.empty()) { // the owner may have finished the very file we want
        try {
            if (sha256_file(j.dest) == expected) {
                download_result r;
                r.ok = true;
                r.skipped = true;
                r.status = 200;
                r.sha256 = expected;
                return r;
            }
        } catch (const std::exception&) { // not there (yet): download it ourselves
        }
    }

    bool force = j.force;
    for (int attempt = 0;; ++attempt) {
        download_result r = j.resume ? run_resumable(j.url, j.dest, j.progress, j.follow, force, j.timeout_seconds)
                                     : run_plain(j.url, j.dest, j.progress, j.follow, j.timeout_seconds);
        if (!r.ok || expected.empty()) return r;

        std::string actual;
        try {
            actual = sha256_file(j.dest);
        } catch (const std::exception& e) {
            r.ok = false;
            r.error = std::string("could not check SHA-256: ") + e.what();
            return r;
        }
        r.sha256 = actual;
        if (actual == expected) return r;

        fs::remove_file(j.dest);  // never leave a file that failed its check
        if (j.resume && !force && attempt == 0) {
            force = true;  // a stale or corrupt file / .part was trusted: once more, from byte 0
            continue;
        }
        r.ok = false;
        r.skipped = false;
        r.error = "SHA-256 mismatch: expected " + expected + ", got " + actual + "; the file was deleted";
        return r;
    }
}

} // namespace

download::download(std::string url, std::string dest_path)
    : url_(std::move(url)), dest_path_(std::move(dest_path)) {}

download& download::output(std::string dest_path) {
    dest_path_ = std::move(dest_path);
    return *this;
}

download& download::enable_progress(bool enable) {
    show_progress_ = enable;
    return *this;
}

download& download::disable_progress() {
    return enable_progress(false);
}

download& download::follow_redirects(bool enable) {
    follow_redirects_ = enable;
    return *this;
}

download& download::resume(bool enable) {
    resume_ = enable;
    return *this;
}

download& download::force(bool enable) {
    force_ = enable;
    return *this;
}

download& download::sha256(std::string hex) {
    sha256_ = std::move(hex);
    use_sha256_url_ = false;
    return *this;
}

download& download::sha256_url(std::string checksum_url) {
    sha256_url_ = std::move(checksum_url);
    use_sha256_url_ = true;
    return *this;
}

download& download::set_timeout(long seconds) {
    timeout_seconds_ = seconds;
    return *this;
}

download& download::set_max_wait(std::chrono::milliseconds max_wait) {
    if (max_wait.count() > 0) max_wait_ = max_wait;
    return *this;
}

download_result download::run() const {
    return execute({url_, dest_path_, show_progress_, follow_redirects_, resume_, force_, sha256_, sha256_url_,
                    use_sha256_url_, timeout_seconds_, max_wait_});
}

std::future<download_result> download::run_async() const {
    // The job is a copy: the builder may go out of scope before the async task runs.
    return std::async(std::launch::async, [j = job{url_, dest_path_, show_progress_, follow_redirects_, resume_, force_,
                                                    sha256_, sha256_url_, use_sha256_url_, timeout_seconds_, max_wait_}] {
        return execute(j);
    });
}

} // namespace httpp
