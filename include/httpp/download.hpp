#pragma once

#include "httpp/export.hpp"

#include <chrono>
#include <future>
#include <string>

namespace httpp {

struct download_result {
    bool ok = false;
    // true if run() made no request at all because dest_path already held
    // the complete file (see download::resume()). ok is also true in that
    // case; this just distinguishes "already had it" from "downloaded it
    // just now" for callers that care (e.g. progress reporting).
    bool skipped = false;
    int status = 0;
    std::string error;
    // SHA-256 (64 lowercase hex chars) of the downloaded file; set only when sha256()/sha256_url() asked for
    // verification, and then it is the digest the file was checked against.
    std::string sha256;
};

// Timing of download::lock(); the defaults suit real downloads, tests shrink them.
struct download_lock_options {
    std::chrono::milliseconds heartbeat{1000};    // the process that downloads rewrites <lock>/alive this often
    std::chrono::milliseconds stale_after{15000}; // a lock whose heartbeat has not changed for this long is dead
    std::chrono::milliseconds poll{200};          // how often a waiting process looks at the lock
    std::chrono::milliseconds max_wait{3600000};  // a waiter gives up on a live lock after this and downloads anyway
};

// Fluent builder — the one way to download a URL to a file in httpp:
//
//   httpp::download("http://x.com/f.zip", "f.zip").run();
//
//   httpp::download("http://x.com/f.zip")
//       .output("f.zip")
//       .enable_progress()
//       .run();
//
//   auto fut = httpp::download(url, path).run_async();
//   download_result res = fut.get();
class download {
public:
    // dest_path can be given here (the common case: url + where to save
    // it) or filled in later via output() — e.g. when it isn't known yet
    // at construction time, or alongside other fluent options.
    explicit HTTPP_API download(std::string url, std::string dest_path = "");

    HTTPP_API download& output(std::string dest_path);
    HTTPP_API download& enable_progress(bool enable = true);
    HTTPP_API download& disable_progress();

    // -L/--location. On by default: the common case (GitHub release
    // assets, CDN links, shortened URLs) answers 302 before serving
    // anything, and a plain "download this URL" is expected to land the
    // real file. Pass false to treat a 3xx as the final response instead.
    HTTPP_API download& follow_redirects(bool enable = true);

    // wget -c-style resume, off by default:
    //   - if dest_path already exists and a HEAD request confirms it's
    //     exactly the full size the server reports, run() makes no request
    //     at all and returns {ok=true, skipped=true}.
    //   - otherwise, writes into a same-directory "<dest_path>.part" file;
    //     if that .part already has bytes from an earlier attempt, resumes
    //     with a Range request from where it left off instead of
    //     restarting. .part is renamed to dest_path only once the download
    //     actually completes, so dest_path itself is never left truncated
    //     or partially written by a failed/interrupted run.
    //   - if the server doesn't honor the Range request (responds 200
    //     instead of 206), falls back to a full restart transparently.
    //   - requires the server to answer HEAD and Range requests; if it
    //     doesn't, resume degrades to the same always-fresh behavior as
    //     when this is off.
    HTTPP_API download& resume(bool enable = true);

    // Ignore any of the above (a complete dest_path, or a partial .part)
    // and always redownload from byte 0. Has no effect unless resume() is
    // also enabled, since that's the only mode that looks at existing
    // files in the first place.
    HTTPP_API download& force(bool enable = true);

    // Verify the finished file against this SHA-256 (64 hex digits, any case). The check happens before run()
    // reports success: on a mismatch the file is deleted and ok is false, with the digest it got in `error`.
    // A malformed digest fails at once, before any request. With resume(), a file that is already there or a
    // .part that was resumed is hashed too; if it does not match, it is downloaded again once from byte 0.
    HTTPP_API download& sha256(std::string hex);

    // Read the expected SHA-256 from a checksum file instead: one line as `sha256sum` writes it, i.e.
    // `<64 hex digits>` optionally followed by the file name (which must be the name in the download URL).
    // `checksum_url` empty means `<download url>.sha256`. It is fetched before the file, so a missing or malformed
    // checksum file (an HTML error page, say) fails the call without downloading anything. Overrides sha256().
    HTTPP_API download& sha256_url(std::string checksum_url = "");

    // Only one process (or thread) at a time downloads a given dest_path; the others wait, then carry on, so
    // several workers that want the same file do not fetch it several times.
    //   - the lock is the directory "<dest_path>.lock"; the owner rewrites "<dest_path>.lock/alive" every
    //     `heartbeat`. A waiter that sees that file unchanged for `stale_after` knows the owner died and takes
    //     the lock over; after `max_wait` it stops waiting and downloads anyway.
    //   - a waiter that finds dest_path complete and matching sha256()/sha256_url() uses it without a request
    //     (skipped=true). Pair lock() with resume() or a checksum: without either, a waiter downloads the file
    //     again once the lock is free.
    //   - if the lock directory cannot be created (read-only directory) the download goes ahead unlocked.
    //   - the lock only saves work; it does not make overlapping writers safe. Two waiters that take over the
    //     same dead lock at the same instant (or a waiter that hit max_wait) can write dest_path together: use
    //     sha256() so a damaged result is caught and deleted.
    HTTPP_API download& lock(download_lock_options options = {});

    HTTPP_API download_result run() const;
    HTTPP_API std::future<download_result> run_async() const;

private:
    std::string url_;
    std::string dest_path_;
    bool show_progress_ = true;
    bool follow_redirects_ = true;
    bool resume_ = false;
    bool force_ = false;
    std::string sha256_;
    std::string sha256_url_;
    bool use_sha256_url_ = false;
    bool use_lock_ = false;
    download_lock_options lock_options_;
};

} // namespace httpp
