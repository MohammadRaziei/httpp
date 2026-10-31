// The C++ side of the benchmark suite: the same answers as the Python apps, through each way of registering a route.
//   cpp_bench PORT [IO_THREADS]
// /ws is a WebSocket echo (the same as the Python apps and drogon_bench).
#include "httpp/server.hpp"
#include "httpp/websocket.hpp"

#include <cstdlib>

namespace {

void hello(httpp::response& r) {
    r.body = "hello";
    r.headers.emplace_back("Content-Type", "text/plain");
}

// The same work as the Python /cpu route.
long long work() {
    long long s = 0;
    for (long long i = 0; i < 2000; ++i) s += i * i;
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const int port = std::atoi(argv[1]);
    const std::size_t io_threads = argc > 2 ? static_cast<std::size_t>(std::atoi(argv[2])) : 1;
    httpp::server srv;
    srv.set_io_threads(io_threads);
    srv.route("GET", "/sync", [](const httpp::request&, httpp::response& r) { hello(r); });
    srv.route("GET", "/cpu", [](const httpp::request&, httpp::response& r) {
        volatile long long s = work();
        r.body = std::to_string(s);
    });
    srv.route_io("GET", "/io", [](std::shared_ptr<const httpp::request>, httpp::responder rs) {
        httpp::response r;
        hello(r);
        rs.send(std::move(r));
    });
    httpp::ws_handler echo;
    echo.on_message = [](const httpp::websocket& ws, std::string_view m, bool binary) {
        if (binary) ws.send_binary(m);
        else ws.send_text(m);
    };
    srv.route_ws("/ws", std::move(echo));
    return srv.listen("127.0.0.1", port) ? 0 : 1;
}
