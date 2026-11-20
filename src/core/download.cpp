#include "httpp/download.hpp"
#include "httpp/file_lock.hpp"
#include "httpp/url.hpp"
#include "httpp/progress.hpp"
#include "httpp/sha256.hpp"

#include "internal/http_exchange.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <future>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace httpp {

namespace {

// The size of a regular file, or -1 (missing, a directory, ...).
long long file_size_or_negative(const std::string& file) {
    const httpp::path::status i = httpp::path(file).stat();
    return i.type == httpp::path::kind::file ? i.size : -1;
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
        out_.open(httpp::path(path_).native(), std::ios::binary | (offset_ > 0 ? std::ios::app : std::ios::trunc));
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

// The request every download step makes: the settings of the download (redirects, timeouts, credentials, proxy, TLS),
// identity coding (the bytes on disk are what the server sent, and a checksum is over those), and `extra` headers.
bool make_transfer_spec(const std::string& url_str, const char* method, const detail::session_settings& net,
                        const detail::header_list& extra, detail::request_spec& spec, std::string& error) {
    detail::session_settings settings = net;
    settings.auto_compression = false;
    detail::header_list headers{{"Accept-Encoding", "identity"}};
    headers.insert(headers.end(), extra.begin(), extra.end());
    response bad;
    if (settings.make_spec_for_url(url_str, "", method, headers, {}, {}, spec, bad)) return true;
    error = "invalid or unsupported URL: " + url_str;
    return false;
}

transfer_result transfer(const std::string& url_str, const detail::session_settings& net, long long range_from, file_target& target) {
    transfer_result result;
    detail::header_list extra;
    if (range_from > 0) extra.emplace_back("Range", "bytes=" + std::to_string(range_from) + "-");
    detail::request_spec spec;
    if (!make_transfer_spec(url_str, "GET", net, extra, spec, result.error)) return result;
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
long long probe_size(const std::string& url_str, const detail::session_settings& net) {
    detail::request_spec spec;
    std::string error;
    if (!make_transfer_spec(url_str, "HEAD", net, {}, spec, error)) return -1;
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
                          const detail::session_settings& net) {
    file_target target(dest_path, false, 0, -1, show_progress);
    const transfer_result t = transfer(url_str, net, 0, target);
    // A failed download must not leave a 0-byte (or half-written) file at dest_path: callers reasonably treat
    // "the file exists" as "it worked".
    if (!t.ok) httpp::path(dest_path).unlink();
    return to_result(t);
}

// resume()/force()-aware implementation: skip-if-complete, stage into dest_path + ".part", and Range-resume that
// .part if it already has bytes from an earlier attempt. See download::resume() in the header for the contract.
download_result run_resumable(const std::string& url_str, const std::string& dest_path, bool show_progress,
                              const detail::session_settings& net, bool force) {
    download_result result;
    if (!url_is_http(parse_url(url_str))) {
        result.error = "invalid or unsupported URL: " + url_str;
        return result;
    }

    // Find out the full size up front: both "already complete, skip" and "is this .part already the full file"
    // are checked against it. Without it resume/skip have nothing to compare with: do a plain download.
    const long long total_size = probe_size(url_str, net);
    if (total_size < 0) return run_plain(url_str, dest_path, show_progress, net);

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
        if (httpp::path(part_path).rename(dest_path)) {
            result.ok = true;
            result.status = 200;
            return result;
        }
        offset = 0; // rename failed; restart cleanly instead of erroring out
    }

    file_target target(part_path, true, offset, total_size, show_progress);
    const transfer_result t = transfer(url_str, net, offset, target);
    result = to_result(t);
    if (t.ok) {
        result.ok = httpp::path(part_path).rename(dest_path);
        if (!result.ok) result.error = "downloaded but could not rename " + part_path + " to " + dest_path;
    } else if (!t.real_content) {
        httpp::path(part_path).unlink(); // never got real content (an error page, no connection): .part is not valid
    }
    return result;
}

// Everything one download needs, copied out of the builder so run_async() can outlive it.
struct job {
    std::string url, dest;
    bool progress = true;
    detail::session_settings net; // timeout 0: the default (300 s) for the network, 15 s for the lock
    bool resume = false, force = false;
    std::string sha256, sha256_url;
    bool use_sha256_url = false;
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

bool same_host_and_port(const std::string& a, const std::string& b) {
    try {
        const url ua = parse_url(a), ub = parse_url(b);
        return url_scheme(ua) == url_scheme(ub) && url_host(ua) == url_host(ub) && url_port(ua) == url_port(ub);
    } catch (const std::exception&) {
        return false;
    }
}

// The digest the file must have, lowercase; empty if no verification was asked for. On a problem returns false.
bool expected_digest(const job& j, std::string& digest, std::string& error) {
    digest.clear();
    if (j.use_sha256_url) {
        const std::string sum_url = j.sha256_url.empty() ? without_query(j.url) + ".sha256" : j.sha256_url;
        const std::string tmp = j.dest + ".sha256.part";
        detail::session_settings net = j.net;
        if (!same_host_and_port(j.url, sum_url)) { // never hand the credentials of the file to another server
            net.has_basic = false;
            net.user.clear();
            net.password.clear();
            net.bearer.clear();
        }
        const download_result got = run_plain(sum_url, tmp, false, net);
        if (!got.ok) {
            httpp::path(tmp).unlink();
            error = "could not download the checksum file " + sum_url + ": " + got.error;
            return false;
        }
        std::string text;
        try {
            text = httpp::path(tmp).read_text(0, 1025);  // one short line; more is read only to reject it
        } catch (const std::runtime_error&) {
        }
        httpp::path(tmp).unlink();
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
    const long timeout = j.net.timeout > 0 ? j.net.timeout : 15;
    const auto stale_after = std::min(std::chrono::milliseconds(timeout * 1000), j.max_wait);
    const auto lock = std::make_unique<httpp::file_lock>(httpp::path(j.dest), stale_after);

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
        download_result r = j.resume ? run_resumable(j.url, j.dest, j.progress, j.net, force)
                                     : run_plain(j.url, j.dest, j.progress, j.net);
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

        httpp::path(j.dest).unlink();  // never leave a file that failed its check
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

struct download::impl {
    detail::guarded_settings settings;
    std::string url, dest;
    bool show_progress = true;
    bool resume = false, force = false;
    std::string sha256, sha256_url;
    bool use_sha256_url = false;
    std::chrono::milliseconds max_wait{2000};

    static detail::session_settings defaults() {
        detail::session_settings s; // redirects are followed (20), unlike request; identity coding, see make_transfer_spec
        s.follow = true;
        s.auto_compression = false;
        return s;
    }
    impl(std::string u, std::string d) : settings(defaults()), url(std::move(u)), dest(std::move(d)) {}

    // Everything run() and run_async() need, copied out of the builder so run_async() can outlive it.
    job to_job() const {
        return job{url, dest, show_progress, settings.snapshot(), resume, force, sha256, sha256_url, use_sha256_url, max_wait};
    }
};

download::download(const url& source, const path& dest_path) : impl_(std::make_unique<impl>(source.str(), dest_path.str())) {}
download::~download() = default;
download::download(download&&) noexcept = default;
download& download::operator=(download&&) noexcept = default;

detail::settings_ref download::settings_() const { return detail::settings_ref(impl_->settings); }

download& download::output(const path& dest_path) & {
    impl_->dest = dest_path.str();
    return *this;
}

download& download::enable_progress(bool enable) & {
    impl_->show_progress = enable;
    return *this;
}

download& download::disable_progress() & {
    return enable_progress(false);
}

download& download::resume(bool enable) & {
    impl_->resume = enable;
    return *this;
}

download& download::force(bool enable) & {
    impl_->force = enable;
    return *this;
}

download& download::sha256(std::string hex) & {
    impl_->sha256 = std::move(hex);
    impl_->use_sha256_url = false;
    return *this;
}

download& download::sha256_url(const url& checksum_url) & {
    impl_->sha256_url = checksum_url.str();
    impl_->use_sha256_url = true;
    return *this;
}

download& download::max_wait(std::chrono::milliseconds wait) & {
    if (wait.count() > 0) impl_->max_wait = wait;
    return *this;
}
std::chrono::milliseconds download::max_wait() const { return impl_->max_wait; }

download_result download::run() const { return execute(impl_->to_job()); }

std::future<download_result> download::run_async() const {
    // The job is a copy: the builder may go out of scope before the async task runs.
    return std::async(std::launch::async, [j = impl_->to_job()] { return execute(j); });
}

} // namespace httpp
