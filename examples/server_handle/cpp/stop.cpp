#include <httpp.h>

#include <chrono>

int main() {
    httpp::server srv;

    // capture a stopper, never the server itself: a handler that holds the server makes a cycle
    srv.get("/quit", [stop = srv.stopper()](const httpp::server_request&, httpp::response& res) {
        stop.shutdown(std::chrono::seconds(2));
        res.body = "bye";
    });

    srv.listen("127.0.0.1", 8000);
}
