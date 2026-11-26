# request_data

What a handler can read from the request: method, path, query, headers and body.

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/server/request-data/](https://mohammadraziei.github.io/httpp/docs/server/request-data/)

## Run

```bash
python python/main.py       # Python
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

It listens on `127.0.0.1:8000`. In another terminal:

```bash
curl -X POST "http://127.0.0.1:8000/inspect?page=2" -H "X-Token: abc" -d hello
curl -F title=shopping -F doc=@notes.txt http://127.0.0.1:8000/upload
```

The second command sends a form with a file to `/upload`: see [`upload`](../upload/) for the client that does the same.

The tests in `tests/python/test_examples.py` run every file here and check what it does.
