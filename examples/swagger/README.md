# swagger

A small API with a Swagger UI page at `/docs` and its OpenAPI document at `/openapi.json`. The document is written by hand with `openapi::document`, which both languages have.

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/server/swagger/](https://mohammadraziei.github.io/httpp/docs/server/swagger/)

## Run

```bash
python python/main.py       # Python
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

It listens on `127.0.0.1:8000`. In another terminal:

```bash
# open http://127.0.0.1:8000/docs in a browser, or:
curl http://127.0.0.1:8000/openapi.json
```

## More files

- `auto_document` (Python): the Library API: the `Server` writes the document itself, from the routes, the type hints and the models. C++ does not read handlers, so it has no twin.

The tests in `tests/python/test_examples.py` run every file here and check what it does.
