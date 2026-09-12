#pragma once

#include "httpp/export.hpp"

#include <future>
#include <string>

namespace httpp {

struct HTTPP_API download_result {
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
class HTTPP_API download_file {
public:
    explicit download_file(std::string url);

    download_file& output(std::string dest_path);
    download_file& enable_progress(bool enable = true);
    download_file& disable_progress();

    download_result run() const;
    std::future<download_result> run_async() const;

private:
    std::string url_;
    std::string dest_path_;
    bool show_progress_ = true;
};

} // namespace httpp
