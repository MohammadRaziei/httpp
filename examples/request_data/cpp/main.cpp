#include <httpp.h>

int main() {
    httpp::server app;

    app.post("/inspect", [](const httpp::server_request& req, httpp::response& res) {
        res.body = req.method + " " + req.path + "\n"
                 + "page=" + req.query_param("page") + "\n"
                 + "token=" + req.header("X-Token") + "\n"
                 + "body=" + req.body;
    });

    app.post("/upload", [](const httpp::server_request& req, httpp::response& res) { // a multipart/form-data body: text fields and files
        const httpp::form_data form = req.form();
        const httpp::upload_file* doc = form.file("doc");
        if (!doc) {
            res.status = 400;
            res.body = "no file named doc";
            return;
        }
        res.body = "title=" + form.field("title") + "\n"
                 + "name=" + doc->filename + "\n"
                 + "type=" + doc->content_type + "\n"
                 + "size=" + std::to_string(doc->data.size());
    });

    app.listen("127.0.0.1", 8000);
}
