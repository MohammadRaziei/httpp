#include <httpp.h>

int main() {
    auto srv = httpp::directory("public").show_hidden(false)
                   .mount(httpp::server(), "/")
                   .tls("cert.pem", "key.pem");

    srv.listen("127.0.0.1", 8000);
}
