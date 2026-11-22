#include <httpp.h>

int main() {
    httpp::server app;

    app.get("/users/{id}", [](const httpp::server_request& req, httpp::response& res) {
        if (req.path_param("id") != "1")
            throw httpp::http_exception(404, "no such user");  // the client gets {"detail": "no such user"} and 404
        res.json("{\"id\":1,\"name\":\"ada\"}");
    });

    app.get("/admin", [](const httpp::server_request&, httpp::response&) {
        throw httpp::http_exception(401, "no token", {{"WWW-Authenticate", "Bearer"}});
    });

    app.listen("127.0.0.1", 8000);
}
