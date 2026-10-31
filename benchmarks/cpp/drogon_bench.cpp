// Drogon, the other C++ web framework, answering the same routes as cpp_bench.cpp (and a WebSocket echo at /ws).
//   drogon_bench PORT [IO_THREADS]
// ponytail: handlers registered directly, Drogon's defaults otherwise (it adds a Server and a Date header like httpp does).
#include <drogon/WebSocketController.h>
#include <drogon/drogon.h>

#include <cstdlib>
#include <string>

namespace {

long long work() {
    long long s = 0;
    for (long long i = 0; i < 2000; ++i) s += i * i;
    return s;
}

class echo : public drogon::WebSocketController<echo> {
public:
    void handleNewMessage(const drogon::WebSocketConnectionPtr& c, std::string&& message, const drogon::WebSocketMessageType& type) override {
        if (type == drogon::WebSocketMessageType::Text || type == drogon::WebSocketMessageType::Binary) c->send(message, type);
    }
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr&) override {}
    void handleNewConnection(const drogon::HttpRequestPtr&, const drogon::WebSocketConnectionPtr&) override {}
    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws");
    WS_PATH_LIST_END
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const int port = std::atoi(argv[1]);
    const int io_threads = argc > 2 ? std::atoi(argv[2]) : 1;
    using namespace drogon;
    app().setLogLevel(trantor::Logger::kError);
    app().registerHandler("/sync", [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& done) {
        auto r = HttpResponse::newHttpResponse();
        r->setContentTypeCode(CT_TEXT_PLAIN);
        r->setBody("hello");
        done(r);
    }, {Get});
    app().registerHandler("/cpu", [](const HttpRequestPtr&, std::function<void(const HttpResponsePtr&)>&& done) {
        volatile long long s = work();
        auto r = HttpResponse::newHttpResponse();
        r->setContentTypeCode(CT_TEXT_PLAIN);
        r->setBody(std::to_string(s));
        done(r);
    }, {Get});
    app().addListener("127.0.0.1", static_cast<uint16_t>(port)).setThreadNum(static_cast<std::size_t>(io_threads)).run();
    return 0;
}
