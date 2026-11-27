# swagger

A small API with a Swagger UI page at `/docs` and its OpenAPI document at `/openapi.json`. The document is written by hand with `openapi::document`, which both languages have.

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/server/swagger/](https://mohammadraziei.github.io/httpp/docs/server/swagger/)

## Run

```bash
python python/main.py       # Python
python python/auto_document.py   # the Library API
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

It listens on `127.0.0.1:8000`. In another terminal:

```bash
# open http://127.0.0.1:8000/docs in a browser, or:
curl http://127.0.0.1:8000/openapi.json
```

## More files

- `auto_document` (Python and C++): the Library API of the pictures on the page. Python's `Server` writes the document itself, from the routes, the type hints and the models; the C++ file writes the same document by hand (`openapi::document`), so the page shows the same thing. Adding a book is not in the C++ file: httpp does not read JSON.

The tests in `tests/python/test_examples.py` run every file here and check what it does.
