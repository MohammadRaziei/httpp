#include <httpp.h>
#include <httpp/file_browser.hpp>  // not part of httpp.h

int main() {
    httpp::server app;

    httpp::file_browser_options options;
    options.title = "Team files";
    httpp::mount_file_browser(app, "/files", "shared", options);

    app.listen("127.0.0.1", 8000);
}
