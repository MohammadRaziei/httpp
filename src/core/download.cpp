#include "httpp/download.hpp"
#include "httpp/url.hpp"
#include "httpp/progress.hpp"

// httplib.h (+ mbedtls support) is included ONLY via this internal header,
// never in a public httpp/*.hpp header — see its comment for why.
#include "internal/httplib_common.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <utility>

namespace httpp {

namespace {

// Portable (pre-C++17-filesystem-availability-safe — see the note on
// macOS 10.13 in discard_partial_file below) "does this file exist, and
// how big is it" check. Uses ifstream::tellg rather than fseek/ftell: the
// latter returns a 32-bit long on Windows even in 64-bit builds, which
// would silently misreport files over 2GB.
long long file_size_or_negative(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return -1;
    }
    return static_cast<long long>(f.tellg());
}

download_result download_resumable(const std::string& url_str, const std::string& dest_path,
                                   bool show_progress, bool follow_redirects, bool force) {
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
    cli.set_follow_location(follow_redirects);

    // Find out the full size up front: this is what both "already
    // complete, skip" and "is this .part actually the full file already"
    // are checked against. If the server won't answer HEAD or doesn't
    // report Content-Length, resume/skip aren't possible — fall back to
    // the plain always-fresh implementation rather than guessing.
    auto head_res = cli.Head(path);
    long long total_size = -1; // -1: unknown
    if (head_res && head_res->status >= 200 && head_res->status < 300 &&
        head_res->has_header("Content-Length")) {
        total_size = std::atoll(head_res->get_header_value("Content-Length").c_str());
    }
    if (total_size < 0) {
        return download(url_str, dest_path, show_progress, follow_redirects);
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
        std::remove(dest_path.c_str());
        if (std::rename(part_path.c_str(), dest_path.c_str()) == 0) {
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

    // Set once headers arrive, before any body bytes are read — this is
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
            // Never got past the response headers with useful status —
            // whatever is in .part (if anything) isn't valid content.
            std::remove(part_path.c_str());
        }
        result.status = response_status;
        result.error = "request failed (connection error)";
        return result;
    }

    result.status = res->status;
    if (res->status == 200 || res->status == 206) {
        result.ok = (std::rename(part_path.c_str(), dest_path.c_str()) == 0);
        if (!result.ok) {
            result.error = "downloaded but could not rename " + part_path + " to " + dest_path;
        }
    } else {
        std::remove(part_path.c_str()); // an error page, not real content — don't keep it
        result.error = "request failed with status " + std::to_string(res->status);
    }
    return result;
}

} // namespace

download_result download(const std::string& url_str, const std::string& dest_path, bool show_progress,
                         bool follow_redirects) {
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
    // Without this a 302 is reported as the final status and the caller is
    // left with a 0-byte file — which is what GitHub release assets, CDN
    // links and shortened URLs all hand back before the real content.
    // httplib suppresses the content receiver and progress callback for the
    // intermediate 3xx responses, so only the final body reaches the file.
    cli.set_follow_location(follow_redirects);

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
    // and a stale partial file also defeats naive resume/skip logic.
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

// --- download_file: fluent builder over download() ---

download_file::download_file(std::string url) : url_(std::move(url)) {}

download_file& download_file::output(std::string dest_path) {
    dest_path_ = std::move(dest_path);
    return *this;
}

download_file& download_file::enable_progress(bool enable) {
    show_progress_ = enable;
    return *this;
}

download_file& download_file::disable_progress() {
    return enable_progress(false);
}

download_file& download_file::follow_redirects(bool enable) {
    follow_redirects_ = enable;
    return *this;
}

download_file& download_file::resume(bool enable) {
    resume_ = enable;
    return *this;
}

download_file& download_file::force(bool enable) {
    force_ = enable;
    return *this;
}

download_result download_file::run() const {
    if (resume_) {
        return download_resumable(url_, dest_path_, show_progress_, follow_redirects_, force_);
    }
    return download(url_, dest_path_, show_progress_, follow_redirects_);
}

std::future<download_result> download_file::run_async() const {
    // Capture by value: the builder (and its url_/dest_path_/show_progress_/
    // follow_redirects_/resume_/force_) may go out of scope before the
    // async task runs.
    return std::async(std::launch::async, [url = url_, dest = dest_path_, show = show_progress_,
                                            follow = follow_redirects_, resume = resume_, force = force_] {
        if (resume) {
            return download_resumable(url, dest, show, follow, force);
        }
        return download(url, dest, show, follow);
    });
}

} // namespace httpp
