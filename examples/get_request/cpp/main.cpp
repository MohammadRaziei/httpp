#include <httpp.h>

#include <iostream>

int main() {
    httpp::client client("127.0.0.1", 8000);
    httpp::response res = client.get("/hello/world");
    std::cout << res.status << " " << res.body << "\n";  // 200 hello world

    // One call with a full URL, no host and port to split:
    std::cout << httpp::client::fetch("http://127.0.0.1:8000/hello/world").status << "\n";
}
