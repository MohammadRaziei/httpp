#include <httpp.h>
#include <httpp/openapi.hpp>
#include <httpp/swagger.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

// The Library API of the Python example `auto_document.py`, with its document written by hand: C++ does not read your handlers, so
// every line that Python takes from a type hint is a line of `doc` here. The page and the document are the same as in Python.

namespace oa = httpp::openapi;

struct book {
    int id;
    std::string title, author;
    int year;
    std::vector<std::string> tags;
};

static std::vector<book> books = {
    {1, "The Pragmatic Programmer", "Andrew Hunt", 1999, {"software", "craft"}},
    {2, "Structure and Interpretation of Computer Programs", "Harold Abelson", 1985, {"software", "lisp"}},
    {3, "A Philosophy of Software Design", "John Ousterhout", 2018, {"software", "design"}},
};

// The titles and names of this example have nothing to escape; real data needs a JSON library (httpp does not read or write JSON).
static std::string to_json(const book& b) {
    std::string tags;
    for (const std::string& t : b.tags) tags += (tags.empty() ? "\"" : ",\"") + t + "\"";
    return "{\"title\":\"" + b.title + "\",\"author\":\"" + b.author + "\",\"year\":" + std::to_string(b.year) + ",\"tags\":[" + tags +
           "],\"id\":" + std::to_string(b.id) + "}";
}

static std::string to_json(const std::vector<book>& list) {
    std::string out = "[";
    for (const book& b : list) out += (out.size() > 1 ? "," : "") + to_json(b);
    return out + "]";
}

static std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Pieces of the document that come back in many places.
static std::string ref(const std::string& name) { return "{\"$ref\":\"#/components/schemas/" + name + "\"}"; }
static std::string json_content(const std::string& schema) { return "{\"application/json\":{\"schema\":" + schema + "}}"; }

static oa::document make_document() {
    oa::document doc("Library API", "1.2.0");
    doc.description("A small API for a library: list, read, add and remove books.");

    // The models, with the rules of their fields. Python writes these from the `BookIn` and `Book` classes.
    const std::string fields =
        "\"title\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":120,\"title\":\"Title\",\"description\":\"The title of the book.\","
        "\"examples\":[\"The Pragmatic Programmer\"]},"
        "\"author\":{\"type\":\"string\",\"minLength\":1,\"title\":\"Author\",\"description\":\"Who wrote it.\",\"examples\":[\"Andrew Hunt\"]},"
        "\"year\":{\"type\":\"integer\",\"minimum\":1450,\"maximum\":2100,\"title\":\"Year\","
        "\"description\":\"The year it was first published.\",\"examples\":[1999]},"
        "\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"title\":\"Tags\",\"description\":\"Free words to find it by.\","
        "\"examples\":[[\"software\",\"craft\"]]}";
    doc.component_schema("BookIn", "{\"properties\":{" + fields + "},\"title\":\"BookIn\",\"type\":\"object\",\"required\":[\"title\",\"author\",\"year\"]}");
    doc.component_schema("Book", "{\"properties\":{" + fields + ",\"id\":{\"type\":\"integer\",\"title\":\"Id\"}},\"title\":\"Book\","
                         "\"type\":\"object\",\"required\":[\"title\",\"author\",\"year\",\"id\"]}");
    doc.component_schema("ValidationError",
                         "{\"properties\":{\"loc\":{\"items\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"integer\"}]},\"type\":\"array\","
                         "\"title\":\"Location\"},\"msg\":{\"type\":\"string\",\"title\":\"Message\"},"
                         "\"type\":{\"type\":\"string\",\"title\":\"Error Type\"}},\"type\":\"object\",\"required\":[\"loc\",\"msg\",\"type\"],"
                         "\"title\":\"ValidationError\"}");
    doc.component_schema("HTTPValidationError",
                         "{\"properties\":{\"detail\":{\"items\":" + ref("ValidationError") + ",\"type\":\"array\",\"title\":\"Detail\"}},"
                         "\"type\":\"object\",\"title\":\"HTTPValidationError\"}");

    const std::string invalid = json_content(ref("HTTPValidationError"));
    const std::string any = json_content("{}");
    const std::string book_id = "{\"type\":\"integer\"}";

    doc.get("/books").tag("books").summary("List books").description("The books in the library, oldest id first.")
        .operation_id("list_books_books_get")
        .parameter("q", "query", false, "{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"null\"}],\"default\":null}",
                   "Only books whose title contains this text.")
        .parameter("limit", "query", false, "{\"type\":\"integer\",\"minimum\":1,\"maximum\":100,\"default\":10}",
                   "How many books to return at most.")
        .response(200, "Successful Response", json_content("{\"type\":\"array\",\"items\":" + ref("Book") + "}"))
        .response(422, "Validation Error", invalid);
    doc.post("/books").tag("books").summary("Add a book").description("Add a book. The id is chosen by the library.")
        .operation_id("add_book_books_post")
        .request_body("{\"required\":true,\"content\":" + json_content(ref("BookIn")) + "}")
        .response(201, "Successful Response", json_content(ref("Book")))
        .response(422, "Validation Error", invalid);
    doc.get("/books/{book_id}").tag("books").summary("Read one book").description("One book by its id. Answers 404 when there is none.")
        .operation_id("read_book_books__book_id__get")
        .parameter("book_id", "path", true, book_id)
        .response(200, "Successful Response", json_content(ref("Book")))
        .response(422, "Validation Error", invalid);
    doc.del("/books/{book_id}").tag("books").summary("Remove a book").operation_id("remove_book_books__book_id__delete")
        .parameter("book_id", "path", true, book_id)
        .response(204, "Successful Response", any)
        .response(422, "Validation Error", invalid);
    doc.get("/books/by-author/{author}").tag("books").summary("Find books by an author").description("Use `GET /books?q=` instead.")
        .operation_id("by_author_books_by_author__author__get").deprecated()
        .parameter("author", "path", true)
        .response(200, "Successful Response", any)
        .response(422, "Validation Error", invalid);
    doc.get("/health").tag("service").summary("Is the library up?").operation_id("health_health_get")
        .response(200, "Successful Response", any);
    return doc;
}

int main() {
    httpp::server app;

    app.get("/books", [](const httpp::server_request& req, httpp::response& res) {
        const std::string q = lower(req.query_param("q"));
        const std::string text = req.query_param("limit");
        const int limit = text.empty() ? 10 : std::atoi(text.c_str());
        if (limit < 1 || limit > 100) throw httpp::http_exception(422, "limit must be from 1 to 100");
        std::vector<book> found;
        for (const book& b : books)
            if (q.empty() || lower(b.title).find(q) != std::string::npos) found.push_back(b);
        if (found.size() > static_cast<std::size_t>(limit)) found.resize(static_cast<std::size_t>(limit));
        res.json(to_json(found));
    });
    app.get("/books/{book_id}", [](const httpp::server_request& req, httpp::response& res) {
        const int id = req.path_param<int>("book_id");
        for (const book& b : books)
            if (b.id == id) {
                res.json(to_json(b));
                return;
            }
        throw httpp::http_exception(404, "no such book");
    });
    app.post("/books", [](const httpp::server_request&, httpp::response&) {
        // httpp does not read JSON. Parse the body with the library you use, check it, and answer 422 as you like.
        throw httpp::http_exception(501, "this example does not add books");
    });
    app.del("/books/{book_id}", [](const httpp::server_request& req, httpp::response& res) {
        const int id = req.path_param<int>("book_id");
        auto it = std::find_if(books.begin(), books.end(), [id](const book& b) { return b.id == id; });
        if (it == books.end()) throw httpp::http_exception(404, "no such book");
        books.erase(it);
        res.status = 204;
    });
    app.get("/books/by-author/{author}", [](const httpp::server_request& req, httpp::response& res) {
        const std::string author = lower(req.path_param("author"));
        std::vector<book> found;
        for (const book& b : books)
            if (lower(b.author).find(author) != std::string::npos) found.push_back(b);
        res.json(to_json(found));
    });
    app.get("/health", [](const httpp::server_request&, httpp::response& res) { res.json("{\"status\":\"ok\"}"); });

    const std::string document = make_document().to_json();
    app.get("/openapi.json", [document](const httpp::server_request&, httpp::response& res) { res.json(document); });
    httpp::swagger("/openapi.json").title("Library API").mount(app, "/docs");

    app.listen("127.0.0.1", 8000);
}
