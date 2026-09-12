#include "httpp/download.hpp"
#include "httpp/url.hpp"
#include "httpp/progress.hpp"

// httplib.h is included ONLY in this translation unit — never in a public
// httpp/*.hpp header — and stays hidden inside the compiled core.
#include <httplib.h>

#include <fstream>
#include <memory>
#include <utility>

namespace httpp {

download_result download(const std::string& url_str, const std::string& dest_path, bool show_progress) {
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

    httplib::Client cli(u.host(), u.port());
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

    if (!res) {
        result.error = "request failed (connection error)";
        return result;
    }

    result.status = res->status;
    result.ok = res->status >= 200 && res->status < 300;
    if (!result.ok) {
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

download_result download_file::run() const {
    return download(url_, dest_path_, show_progress_);
}

std::future<download_result> download_file::run_async() const {
    // Capture by value: the builder (and its url_/dest_path_/show_progress_)
    // may go out of scope before the async task runs.
    return std::async(std::launch::async, [url = url_, dest = dest_path_, show = show_progress_] {
        return download(url, dest, show);
    });
}

} // namespace httpp
