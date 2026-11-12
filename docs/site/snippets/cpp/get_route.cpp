#include <httpp.h>

int main() {
    httpp::server app;

    app.get("/hello/:name", [](const httpp::server_request& req, httpp::response& res) {
        res.body = "hello " + req.path_param("name");
    });

    app.listen("127.0.0.1", 8000);  // blocks until app.stop()
}
