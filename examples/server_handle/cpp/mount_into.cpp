#include <httpp.h>

int main() {
    httpp::server app;
    httpp::directory("public").mount(app, "/static");  // `app` now serves /static

    app.get("/ping", [](const httpp::server_request&, httpp::response& res) { res.body = "pong"; });

    app.listen("127.0.0.1", 8000);
}
