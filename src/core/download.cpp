#include "httpp/download.hpp"
#include "httpp/url.hpp"
#include "httpp/progress.hpp"
#include "httpp/sha256.hpp"

// httplib.h (+ mbedtls support) is included ONLY via this internal header,
// never in a public httpp/*.hpp header - see its comment for why.
#include "internal/httplib_common.hpp"

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
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // MoveFileExA, see replace_file()
#include <direct.h>  // _mkdir, _rmdir (the lock)
#include <process.h> // _getpid
#else
#include <sys/stat.h> // mkdir
#include <unistd.h>   // rmdir, getpid
#endif

namespace httpp {

namespace {

// Portable (pre-C++17-filesystem-availability-safe - see the note on
// macOS 10.13 in run_plain() below) "does this file exist, and how big is
// it" check. Uses ifstream::tellg rather than fseek/ftell: the latter
// returns a 32-bit long on Windows even in 64-bit builds, which would
// silently misreport files over 2GB.
// Moves `from` over `to`, replacing it. std::rename does that on POSIX but fails when `to` exists on Windows, which made
// every download over an existing file (force, or a finished .part) fail there; MoveFileEx replaces it, atomically.
// ponytail: the narrow (ANSI code page) call matches what std::rename did; upgrade path for non-ANSI paths is
// MoveFileExW on a UTF-16 conversion of the UTF-8 path.
bool replace_file(const std::string& from, const std::string& to) {
#ifdef _WIN32
    return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

long long file_size_or_negative(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return -1;
    }
    return static_cast<long long>(f.tellg());
}

// Always-fresh implementation (no HEAD probe, no resume, no skip): the
// server's full body streams straight to dest_path, truncating whatever
// was there before. Used directly when resume() is off, and as the
// fallback from run_resumable() when a HEAD probe can't establish the
// server-reported total size (Content-Length missing/unavailable), since
// resume/skip have nothing to compare against in that case.
void apply_timeout(httplib::Client& cli, long seconds) {
    if (seconds <= 0) return; // cpp-httplib's defaults
    cli.set_connection_timeout(seconds);
    cli.set_read_timeout(seconds);
    cli.set_write_timeout(seconds);
}

download_result run_plain(const std::string& url_str, const std::string& dest_path, bool show_progress,
                          bool follow_redirects, long timeout_seconds) {
    download_result result;

    const url u = url::parse(url_str);
    if (!u.valid()) {
        result.error = "invalid or unsupported URL: " + url_str;
        return result;
    }

    std::ofstream out(dest_path, std::ios::binary);
    if (!out) {
        result.error = "could not open destination file: " + dest_path;
        return result;
    }

    std::string path = u.path();
    if (!u.query().empty()) {
        path += "?" + u.query();
    }

    httplib::Client cli(detail::scheme_host_port(u.scheme(), u.host(), u.port()));
    cli.set_decompress(false); // save exactly what the server sends (a checksum is over those bytes); no zlib here
    // Without this a 302 is reported as the final status and the caller is
    // left with a 0-byte file - which is what GitHub release assets, CDN
    // links and shortened URLs all hand back before the real content.
    // httplib suppresses the content receiver and progress callback for the
    // intermediate 3xx responses, so only the final body reaches the file.
    cli.set_follow_location(follow_redirects);
    apply_timeout(cli, timeout_seconds);

    std::unique_ptr<progress::bar> pb;

    auto res = cli.Get(
        path,
        [&](const char* data, std::size_t len) {
            out.write(data, static_cast<std::streamsize>(len));
            return static_cast<bool>(out);
        },
        [&](std::size_t current, std::size_t total) {
            if (show_progress && total > 0) {
                if (!pb) {
                    pb = std::make_unique<progress::bar>(total, "downloading");
                }
                pb->set_progress(current);
            }
            return true;
        });

    if (pb) {
        pb->finish();
    }

    // A failed download must not leave a 0-byte (or half-written) file at
    // dest_path: callers reasonably treat "the file exists" as "it worked",
    // and a stale partial file also defeats naive resume/skip logic. (Plain
    // std::remove from <cstdio>, not std::filesystem::remove: the latter is
    // marked unavailable below macOS 10.15 in libc++, and this project
    // targets 10.13.)
    auto discard_partial_file = [&] {
        out.close();
        std::remove(dest_path.c_str()); // best-effort; ignore failure
    };

    if (!res) {
        discard_partial_file();
        result.error = "request failed (connection error)";
        return result;
    }

    result.status = res->status;
    result.ok = res->status >= 200 && res->status < 300;
    if (!result.ok) {
        discard_partial_file();
        result.error = "request failed with status " + std::to_string(res->status);
    }
    return result;
}

// resume()/force()-aware implementation: skip-if-complete, stage into
// dest_path + ".part", and Range-resume that .part if it already has bytes
// from an earlier attempt. See download::resume() in the header for the
// full behavior contract.
download_result run_resumable(const std::string& url_str, const std::string& dest_path, bool show_progress,
                              bool follow_redirects, bool force, long timeout_seconds) {
    download_result result;

    const url u = url::parse(url_str);
    if (!u.valid()) {
        result.error = "invalid or unsupported URL: " + url_str;
        return result;
    }

    std::string path = u.path();
    if (!u.query().empty()) {
        path += "?" + u.query();
    }

    httplib::Client cli(detail::scheme_host_port(u.scheme(), u.host(), u.port()));
    cli.set_decompress(false); // save exactly what the server sends (a checksum is over those bytes); no zlib here
    cli.set_follow_location(follow_redirects);
    apply_timeout(cli, timeout_seconds);

    // Find out the full size up front: this is what both "already
    // complete, skip" and "is this .part actually the full file already"
    // are checked against. If the server won't answer HEAD or doesn't
    // report Content-Length, resume/skip aren't possible - fall back to
    // the plain always-fresh implementation rather than guessing.
    auto head_res = cli.Head(path);
    long long total_size = -1; // -1: unknown
    if (head_res && head_res->status >= 200 && head_res->status < 300 &&
        head_res->has_header("Content-Length")) {
        total_size = std::atoll(head_res->get_header_value("Content-Length").c_str());
    }
    if (total_size < 0) {
        return run_plain(url_str, dest_path, show_progress, follow_redirects, timeout_seconds);
    }

    if (!force && file_size_or_negative(dest_path) == total_size) {
        result.ok = true;
        result.skipped = true;
        result.status = 200;
        return result;
    }

    const std::string part_path = dest_path + ".part";
    long long resumed_offset = force ? -1 : file_size_or_negative(part_path);
    if (resumed_offset < 0) {
        resumed_offset = 0;
    } else if (resumed_offset >= total_size) {
        // .part already holds the full file (e.g. a previous run completed
        // the write but was interrupted before the final rename); nothing
        // left to transfer.
        if (replace_file(part_path, dest_path)) {
            result.ok = true;
            result.status = 200;
            return result;
        }
        resumed_offset = 0; // rename failed; restart cleanly instead of erroring out
    }

    bool resuming = resumed_offset > 0;
    std::ofstream out(part_path, std::ios::binary | (resuming ? std::ios::app : std::ios::trunc));
    if (!out) {
        result.error = "could not open destination file: " + part_path;
        return result;
    }

    httplib::Headers hdrs;
    if (resuming) {
        hdrs.emplace("Range", "bytes=" + std::to_string(resumed_offset) + "-");
    }

    // Set once headers arrive, before any body bytes are read - this is
    // what lets a Range request that the server ignored (200 instead of
    // 206) be caught and corrected before a single byte is (mis)appended.
    int response_status = 0;
    auto response_handler = [&](const httplib::Response& res) {
        response_status = res.status;
        if (resuming && res.status != 206) {
            resuming = false;
            resumed_offset = 0;
            out.close();
            out.open(part_path, std::ios::binary | std::ios::trunc);
        }
        return static_cast<bool>(out);
    };

    std::unique_ptr<progress::bar> pb;
    auto res = cli.Get(
        path, hdrs, response_handler,
        [&](const char* data, std::size_t len) {
            out.write(data, static_cast<std::streamsize>(len));
            return static_cast<bool>(out);
        },
        [&](std::size_t current, std::size_t total) {
            // `current`/`total` are relative to this response's own body
            // (e.g. the *remaining* bytes on a 206), not the whole file;
            // offset both by what a resume already had on disk so the bar
            // reflects the true, total-file progress.
            if (show_progress) {
                auto bar_total = total_size > 0 ? static_cast<std::size_t>(total_size)
                                                : resumed_offset + total;
                if (bar_total > 0) {
                    if (!pb) {
                        pb = std::make_unique<progress::bar>(bar_total, "downloading");
                    }
                    pb->set_progress(static_cast<std::size_t>(resumed_offset) + current);
                }
            }
            return true;
        });
    if (pb) {
        pb->finish();
    }
    out.close();

    const bool got_real_content = response_status == 200 || response_status == 206;
    if (!res) {
        if (!got_real_content) {
            // Never got past the response headers with useful status -
            // whatever is in .part (if anything) isn't valid content.
            std::remove(part_path.c_str());
        }
        result.status = response_status;
        result.error = "request failed (connection error)";
        return result;
    }

    result.status = res->status;
    if (res->status == 200 || res->status == 206) {
        result.ok = replace_file(part_path, dest_path);
        if (!result.ok) {
            result.error = "downloaded but could not rename " + part_path + " to " + dest_path;
        }
    } else {
        std::remove(part_path.c_str()); // an error page, not real content - don't keep it
        result.error = "request failed with status " + std::to_string(res->status);
    }
    return result;
}

// The checksum file is one short line; anything longer is read only as far as parse_sha256_checksum needs to reject it.
std::string read_prefix(const std::string& path, std::size_t limit) {
    std::ifstream in(path, std::ios::binary);
    std::string text(limit, '\0');
    in.read(&text[0], static_cast<std::streamsize>(limit));
    text.resize(static_cast<std::size_t>(in.gcount()));
    return text;
}

// ---- the cross-process lock (download::lock) -------------------------------------------------------------------
// No <filesystem> (see the macOS 10.13 note above): mkdir/rmdir plus plain files are all it needs. Creating a
// directory is atomic everywhere, so whoever creates "<dest>.lock" owns it. Liveness is judged by the waiter's own
// clock from whether `alive` *changes*, not from file times, so clock skew and coarse mtimes do not matter.

// Returns true if the directory was created; `exists` tells "already there" apart from "cannot create it".
bool make_dir(const std::string& path, bool& exists) {
#ifdef _WIN32
    const int r = _mkdir(path.c_str());
#else
    const int r = mkdir(path.c_str(), 0777);
#endif
    exists = r != 0 && errno == EEXIST;
    return r == 0;
}

void remove_dir(const std::string& path) {
#ifdef _WIN32
    _rmdir(path.c_str());
#else
    rmdir(path.c_str());
#endif
}

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
            bool exists = false;
            if (make_dir(path_, exists)) {
                owned_ = true;
                break;
            }
            if (!exists) break; // cannot create it (read-only directory?): go on without a lock
            waited_ = true;
            const std::string seen = read_prefix(path_ + "/alive", 64);
            const auto now = clock::now();
            if (first || seen != last_seen) { // the owner is alive (or this is the first look)
                last_seen = seen;
                last_change = now;
                first = false;
            } else if (now - last_change > stale_after) { // no sign of life for the whole timeout: its owner died
                std::remove((path_ + "/alive").c_str());
                remove_dir(path_);
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
        std::remove((path_ + "/alive").c_str());
        remove_dir(path_);
    }

    dir_lock(const dir_lock&) = delete;
    dir_lock& operator=(const dir_lock&) = delete;

    // true if someone else held the lock at some point, i.e. the file may have been downloaded meanwhile
    bool waited() const { return waited_; }

private:
    void beat(unsigned long long n) const {
        std::ofstream out(path_ + "/alive", std::ios::binary | std::ios::trunc);
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
    long timeout_seconds = 0; // 0: cpp-httplib's defaults for the network, 15 s for the lock
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
            std::remove(tmp.c_str());
            error = "could not download the checksum file " + sum_url + ": " + got.error;
            return false;
        }
        const std::string text = read_prefix(tmp, 1025);
        std::remove(tmp.c_str());
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

        std::remove(j.dest.c_str());  // never leave a file that failed its check
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
