#include <httpp.h>

#include <iostream>

int main() {
    httpp::download_result result = httpp::download("http://127.0.0.1:8000/hello/world", "hello.txt").run();
    std::cout << result.status << "\n";  // 200; hello.txt now holds "hello world"
}
