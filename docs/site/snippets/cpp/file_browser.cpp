#include <httpp.h>
#include <httpp/file_browser.hpp>  // not part of httpp.h

int main() {
    httpp::server app;

    httpp::file_browser("shared").set_title("Team files").mount(app, "/files");

    app.listen("127.0.0.1", 8000);
}
