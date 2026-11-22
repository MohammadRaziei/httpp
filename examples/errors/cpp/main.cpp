#include <httpp.h>

#include <stdexcept>
#include <string>

int main() {
    httpp::server app;

    app.get("/users/{id}", [](const httpp::server_request& req, httpp::response& res) {
        if (req.path_param("id") != "1") {
            res.status = 404;  // your own answer: it is sent as it is
            res.json("{\"error\":\"no such user\"}");
            return;
        }
        res.json("{\"id\":1,\"name\":\"ada\"}");
    });

    app.get("/parse", [](const httpp::server_request&, httpp::response&) {
        throw std::invalid_argument("not a number");  // a throw ends in the exception handler below
    });

    app.get("/boom", [](const httpp::server_request&, httpp::response&) {
        throw std::runtime_error("secret internals");  // nobody handles this: the client sees a plain 500
    });

    app.exception_handler([](const httpp::server_request&, httpp::response& res, std::exception_ptr error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::invalid_argument& e) {
            res.status = 422;
            res.json(std::string("{\"error\":\"") + e.what() + "\"}");
        } catch (...) {
            // anything else keeps the 500 that the server started with
        }
    });

    app.error_handler([](const httpp::server_request&, httpp::response& res) {
        if (res.status == 404) res.json("{\"error\":\"not found\"}");  // the server's own 404, for a path that no route has
    });

    app.listen("127.0.0.1", 8000);
}
