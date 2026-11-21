# responses

Status codes, headers and JSON in responses: create an item with `201` and a `Location`, read it back, get a `404`.

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/server/responses/](https://mohammadraziei.github.io/httpp/docs/server/responses/)

## Run

```bash
python python/main.py       # Python
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

It listens on `127.0.0.1:8000`. In another terminal:

```bash
curl -i -X POST http://127.0.0.1:8000/items
curl -i http://127.0.0.1:8000/items/1
curl -i http://127.0.0.1:8000/items/2     # 404
```

The code is in `python/main.py` and `cpp/main.cpp`; the tests in `tests/python/test_examples.py` run both and check what they do.
