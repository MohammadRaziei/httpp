#include <httpp.h>

#include <iostream>

int main() {
    httpp::download_result result = httpp::download("http://127.0.0.1:8000/hello/world", "hello.txt")
                                        .disable_progress()
                                        .sha256("b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9")
                                        .run();
    if (!result.ok) {
        std::cerr << "download failed: " << result.error << "\n";
        return 1;
    }
    std::cout << result.sha256 << "\n";  // the digest the file was checked against
}
