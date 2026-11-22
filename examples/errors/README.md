# errors

Four ways a request goes wrong and what the client sees: your own `404`, an exception that a handler turns into an answer (`422`), an exception that nobody handles (a plain `500`, the message stays in the server), and the server's own `404` for a path that no route has, shaped by an error handler.

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/server/errors/](https://mohammadraziei.github.io/httpp/docs/server/errors/)

## Run

```bash
python python/main.py       # Python
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

It listens on `127.0.0.1:8000`. In another terminal:

```bash
curl -i http://127.0.0.1:8000/users/9     # 404, your own answer
curl -i http://127.0.0.1:8000/parse       # 422, from the exception handler
curl -i http://127.0.0.1:8000/boom        # 500, the message is not in the answer
curl -i http://127.0.0.1:8000/nope        # 404, from the error handler
```

## More files

- `http_exception` (Python): `raise httpp.HTTPException(404, "no such user")` answers `{"detail": "no such user"}` with that status; C++ has no twin, it sets the status on the response.

The tests in `tests/python/test_examples.py` run every file here and check what it does.
