#include <httpp.h>

int main() {
    httpp::server app;

    app.post("/inspect", [](const httpp::request& req, httpp::response& res) {
        res.body = req.method + " " + req.path + "\n"
                 + "page=" + req.query_param("page") + "\n"
                 + "token=" + req.header("X-Token") + "\n"
                 + "body=" + req.body;
    });

    app.listen("127.0.0.1", 8000);
}
