#include <httpp.h>

int main() {
    httpp::server app;

    app.get("/report", [](const httpp::server_request&, httpp::response& res) {
        res.file = "report.txt";  // streamed from disk
        res.headers.emplace_back("Content-Type", httpp::mime_type_for("report.txt"));
        res.headers.emplace_back("Content-Disposition", "attachment; filename=\"report.txt\"");
    });

    app.listen("127.0.0.1", 8000);
}
