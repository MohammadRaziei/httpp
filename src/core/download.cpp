#include "httpp/download.hpp"
#include "httpp/url.hpp"
#include "httpp/progress.hpp"

// httplib.h (+ mbedtls support) is included ONLY via this internal header,
// never in a public httpp/*.hpp header — see its comment for why.
#include "internal/httplib_common.hpp"

#include <cstdio>
#include <fstream>
#include <memory>
#include <utility>

namespace httpp {

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

download_result download_file::run() const {
    return download(url_, dest_path_, show_progress_, follow_redirects_);
}

std::future<download_result> download_file::run_async() const {
    // Capture by value: the builder (and its url_/dest_path_/show_progress_/
    // follow_redirects_) may go out of scope before the async task runs.
    return std::async(std::launch::async,
                      [url = url_, dest = dest_path_, show = show_progress_, follow = follow_redirects_] {
                          return download(url, dest, show, follow);
                      });
}

} // namespace httpp
