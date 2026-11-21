#include <httpp.h>

int main() {
    httpp::server app;
    app.serve_directory("/static", "public");  // /static/notes.txt is public/notes.txt

    app.listen("127.0.0.1", 8000);
}
