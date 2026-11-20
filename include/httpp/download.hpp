#pragma once

#include "httpp/client.hpp"
#include "httpp/export.hpp"
#include "httpp/path.hpp"
#include "httpp/url.hpp"

#include <chrono>
#include <future>
#include <memory>
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

// Fluent builder - the way to download a URL to a file in httpp, with the file semantics a download needs (the .part file, resume,
// SHA-256, a lock, progress; only a good status is written). To write the answer of one request as it is, an error page included,
// as curl -o does, use request::save_to() (ADR-0102):
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
//
// The network settings (timeout, connect_timeout, max_time, max_redirects, basic_auth, bearer, proxy, verify, ca_file) are the
// ones of client and request, from configurable (ADR-0094, ADR-0101), under the same names. Where a download differs:
//   - max_redirects is 20 by default, not 0: redirects are followed here unlike in request. The common case (GitHub release
//     assets, CDN links, shortened URLs) answers 302 before serving anything, and a plain "download this URL" is expected to
//     land the real file. 0 follows none (a 3xx is the final response), -1 has no limit.
//   - timeout is also how long a download waits for another process that holds the same file's lock while that lock shows
//     no sign of life: see "Locking". 0 or less keeps the default (300 s for the network, 15 s for the lock).
//   - auto_compression is deleted (a call does not compile): a download always asks for identity coding, because the bytes
//     on disk are what the server sent and a checksum is over those.
//   - retry, retry_delay and retry_connrefused work as on request (ADR-0099), each try with its own timeout and max_time.
//     A try that is cut off in the middle of the body does not leave the file wrong (ADR-0104): a 206 is written where its
//     Content-Range says it starts, over what an earlier try already appended, and a 200 starts the file again. A 206 with no
//     usable Content-Range, or one that starts after the end of the file, is an error, not a guess. With resume() the Range
//     is still the one asked for at the start, so a retry takes again the bytes the failed try already wrote.
// A credential is sent to the URL and to the checksum file only when that is on the same host and port as the URL; the
// redirects that are followed keep the rule of request (never to another origin).
//
// Like request, a download is a builder for one job: move-only, and a chain on a temporary is a download.
class download : public configurable<download> {
public:
    // dest_path can be given here (the common case: url + where to save
    // it) or filled in later via output() - e.g. when it isn't known yet
    // at construction time, or alongside other fluent options.
    explicit HTTPP_API download(const url& source, const path& dest_path = path());
    HTTPP_API ~download();

    HTTPP_API download(download&&) noexcept;
    HTTPP_API download& operator=(download&&) noexcept;
    download(const download&) = delete;
    download& operator=(const download&) = delete;

    // Not for a download (see above). Declared here so that the name of configurable is hidden.
    download& auto_compression(bool) = delete;
    bool auto_compression() const = delete;

    HTTPP_API download& output(const path& dest_path) &;
    download output(const path& dest_path) && { output(dest_path); return std::move(*this); }
    HTTPP_API download& enable_progress(bool enable = true) &;
    download enable_progress(bool enable = true) && { enable_progress(enable); return std::move(*this); }
    HTTPP_API download& disable_progress() &;
    download disable_progress() && { disable_progress(); return std::move(*this); }

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
    HTTPP_API download& resume(bool enable = true) &;
    download resume(bool enable = true) && { resume(enable); return std::move(*this); }

    // Ignore any of the above (a complete dest_path, or a partial .part)
    // and always redownload from byte 0. Has no effect unless resume() is
    // also enabled, since that's the only mode that looks at existing
    // files in the first place.
    HTTPP_API download& force(bool enable = true) &;
    download force(bool enable = true) && { force(enable); return std::move(*this); }

    // Verify the finished file against this SHA-256 (64 hex digits, any case). The check happens before run()
    // reports success: on a mismatch the file is deleted and ok is false, with the digest it got in `error`.
    // A malformed digest fails at once, before any request. With resume(), a file that is already there or a
    // .part that was resumed is hashed too; if it does not match, it is downloaded again once from byte 0.
    HTTPP_API download& sha256(std::string hex) &;
    download sha256(std::string hex) && { sha256(std::move(hex)); return std::move(*this); }

    // Read the expected SHA-256 from a checksum file instead: one line as `sha256sum` writes it, i.e.
    // `<64 hex digits>` optionally followed by the file name (which must be the name in the download URL).
    // `checksum_url` empty (the default, url("")) means `<download url>.sha256`. It is fetched before the file, so a missing or malformed
    // checksum file (an HTML error page, say) fails the call without downloading anything. Overrides sha256().
    HTTPP_API download& sha256_url(const url& checksum_url = url()) &;
    download sha256_url(const url& checksum_url = url()) && { sha256_url(checksum_url); return std::move(*this); }

    // How long a lock that is NOT being updated is waited for before its owner is taken for dead and the lock is
    // taken over; default 2 s, values <= 0 are ignored. The wait is min(max_wait, timeout()). It is not a limit on
    // waiting in general: a lock that keeps being updated is waited for as long as it lives.
    HTTPP_API download& max_wait(std::chrono::milliseconds wait) &;
    download max_wait(std::chrono::milliseconds wait) && { max_wait(wait); return std::move(*this); }
    HTTPP_API std::chrono::milliseconds max_wait() const;

    // Locking (always on). Only one process (or thread) at a time downloads a given dest_path; the others wait,
    // then carry on, so several workers that want the same file do not fetch it several times.
    //   - the lock is the directory "<dest_path>.lock"; the owner rewrites "<dest_path>.lock/alive" twice per
    //     wait window (at most every 200 ms). A waiter that sees that file unchanged for min(max_wait(),
    //     timeout()) (2 s and 15 s if unset) knows the owner died and takes the lock over. A lock that keeps
    //     being updated is waited for as long as it lives (the owner's own network timeouts bound that).
    //   - a waiter that finds dest_path complete and matching sha256()/sha256_url() uses it without a request
    //     (skipped=true). Without a checksum (or resume(), which skips a complete file) a waiter downloads the file
    //     again once the lock is free.
    //   - if the lock directory cannot be created (read-only directory) the download goes ahead unlocked.
    //   - the lock only saves work; it does not make overlapping writers safe. Two waiters that take over the same
    //     dead lock at the same instant can write dest_path together: use sha256() so a damaged result is caught
    //     and deleted.

    HTTPP_API download_result run() const;
    HTTPP_API std::future<download_result> run_async() const;

private:
    friend class configurable<download>;
    HTTPP_API detail::settings_ref settings_() const; // what configurable<download> calls; exported because that code is inline
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httpp
