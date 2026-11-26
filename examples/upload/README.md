# upload

Sends a text field and a file as a `multipart/form-data` form and prints the status and what the server read.

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/client/upload/](https://mohammadraziei.github.io/httpp/docs/client/upload/)

It talks to the server of [`request_data`](../request_data/), so start that one first (it listens on `127.0.0.1:8000`).

## Run

```bash
python python/main.py       # Python
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

## More files

- `progress` (Python and C++): the same upload with a callback that is told how many bytes went out.

The tests in `tests/python/test_examples.py` run every file here and check what it does.
