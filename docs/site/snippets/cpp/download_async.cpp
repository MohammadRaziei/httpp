#include <httpp.h>

#include <future>
#include <iostream>

int main() {
    std::future<httpp::download_result> pending =
        httpp::download("http://127.0.0.1:8000/hello/world", "hello.txt").disable_progress().run_async();

    // ... other work runs here while the file comes in ...

    std::cout << pending.get().status << "\n";
}
