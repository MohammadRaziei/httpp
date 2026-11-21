#include <httpp.h>
#include <httpp/openapi.hpp>
#include <httpp/swagger.hpp>

#include <string>

int main() {
    namespace oa = httpp::openapi;
    httpp::server app;

    app.get("/hello/{name}", [](const httpp::server_request& req, httpp::response& res) {
        res.json("{\"hello\":\"" + std::string(req.path_param("name")) + "\"}");
    });
    app.get("/health", [](const httpp::server_request&, httpp::response& res) { res.json("{\"ok\":true}"); });

    oa::document doc("Hello API", "1.0.0");
    doc.security_scheme("jwt", oa::bearer("JWT"));
    doc.security(oa::any_of({"jwt"}));  // the default of every operation
    doc.get("/hello/{name}").summary("Say hello").parameter("name", "path", true).response(200, "A greeting");
    doc.get("/health").no_security().response(200, "OK");  // public although the default asks for a JWT

    app.get("/openapi.json", [&doc](const httpp::server_request&, httpp::response& res) { res.json(doc.to_json()); });
    httpp::swagger("/openapi.json").title("Hello API").mount(app, "/docs");

    app.listen("127.0.0.1", 8000);
}
