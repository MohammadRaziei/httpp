# hooks

A hook that runs before every handler (it refuses a request without the key) and one that runs after (it adds a header).

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/server/hooks/](https://mohammadraziei.github.io/httpp/docs/server/hooks/)

## Run

```bash
python python/main.py       # Python
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

It listens on `127.0.0.1:8000`. In another terminal:

```bash
httpp curl -i http://127.0.0.1:8000/                       # 403
httpp curl -i -H "X-Key: secret" http://127.0.0.1:8000/   # 200, with X-Served-By
```

The tests in `tests/python/test_examples.py` run every file here and check what it does.
