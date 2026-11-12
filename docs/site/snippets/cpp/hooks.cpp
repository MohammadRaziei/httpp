#include <httpp.h>

int main() {
    httpp::server app;

    app.before([](const httpp::server_request& req, httpp::response& res) {
        if (req.header("X-Key") != "secret") {
            res.status = 403;
            res.json("{\"error\":\"forbidden\"}");
            return true;  // answered here; the route never runs
        }
        return false;
    });

    app.after([](const httpp::server_request&, httpp::response& res) {
        res.headers.emplace_back("X-Served-By", "httpp");  // status and headers only
    });

    app.get("/", [](const httpp::server_request&, httpp::response& res) { res.body = "hello"; });

    app.listen("127.0.0.1", 8000);
}
