#include <httpp.h>

#include <fstream>
#include <iostream>
#include <string>

int main() {
    std::ofstream("big.bin", std::ios::binary) << std::string(200000, '\0'); // 200 KB of zeros to send

    httpp::response res = httpp::request("http://127.0.0.1:8000/upload")
                              .field("title", "big")
                              .file("doc", httpp::path("big.bin"))
                              .progress([](std::uint64_t sent, std::uint64_t total) { std::cout << sent << "/" << total << "\n"; })
                              .run();
    std::cout << res.status << "\n";
}
