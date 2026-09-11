#include <httpp.h>
#include <iostream>

int main() {
    httpp::url u = httpp::url::parse("http://example.com/hello");
    std::cout << "httpp found and linked OK. host = " << u.host() << "\n";
    return 0;
}
