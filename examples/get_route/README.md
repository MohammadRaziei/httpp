# get_route

A route with a path parameter: `GET /hello/{name}` answers `hello <name>`.

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/server/get-route/](https://mohammadraziei.github.io/httpp/docs/server/get-route/)

## Run

```bash
python python/main.py       # Python
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

It listens on `127.0.0.1:8000`. In another terminal:

```bash
httpp curl http://127.0.0.1:8000/hello/world            # hello world
```

The tests in `tests/python/test_examples.py` run every file here and check what it does.
