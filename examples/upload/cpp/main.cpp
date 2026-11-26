#include <httpp.h>

#include <fstream>
#include <iostream>

int main() {
    std::ofstream("notes.txt") << "remember the milk\n"; // a small file to send

    httpp::response res = httpp::request("http://127.0.0.1:8000/upload")
                              .field("title", "shopping")
                              .file("doc", httpp::path("notes.txt"))
                              .run();
    std::cout << res.status << "\n" << res.body << "\n";
}
