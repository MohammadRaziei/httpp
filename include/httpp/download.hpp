#pragma once

#include "httpp/export.hpp"

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
};

} // namespace httpp
