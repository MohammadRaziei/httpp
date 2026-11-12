#include <httpp.h>

int main() {
    httpp::server app;

    app.post("/items", [](const httpp::server_request&, httpp::response& res) {
        res.status = 201;
        res.headers.emplace_back("Location", "/items/1");
        res.json("{\"id\":1}");  // you serialize; json() sets Content-Type
    });

    app.get("/items/:item_id", [](const httpp::server_request& req, httpp::response& res) {
        if (req.path_param("item_id") != "1") {
            res.status = 404;
            res.json("{\"error\":\"not found\"}");
            return;
        }
        res.json("{\"id\":1}");
    });

    app.listen("127.0.0.1", 8000);
}
