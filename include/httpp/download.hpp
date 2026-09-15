#pragma once

#include "httpp/export.hpp"

#include <future>
#include <string>

namespace httpp {

struct download_result {
    bool ok = false;
    int status = 0;
    std::string error;
};

// One-shot free function form.
HTTPP_API download_result download(const std::string& url,
                                    const std::string& dest_path,
                                    bool show_progress = true);

// Fluent builder form:
//
//   httpp::download_file("http://x.com/f.zip")
//       .output("f.zip")
//       .enable_progress()
//       .run();
//
//   auto fut = httpp::download_file(url).output(path).run_async();
//   download_result res = fut.get();
class download_file {
public:
    explicit HTTPP_API download_file(std::string url);

    HTTPP_API download_file& output(std::string dest_path);
    HTTPP_API download_file& enable_progress(bool enable = true);
    HTTPP_API download_file& disable_progress();

    HTTPP_API download_result run() const;
    HTTPP_API std::future<download_result> run_async() const;

private:
    std::string url_;
    std::string dest_path_;
    bool show_progress_ = true;
};

} // namespace httpp
