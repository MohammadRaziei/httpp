# download

Downloads a URL to a file and reports the status.

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/client/download/](https://mohammadraziei.github.io/httpp/docs/client/download/)

It talks to the server of [`get_route`](../get_route/), so start that one first (it listens on `127.0.0.1:8000`).

## Run

```bash
python python/main.py       # Python
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

## More files

- `verified` (Python and C++): checks the SHA-256 of the file while it arrives.
- `async` (Python and C++): downloads without blocking: `await ...run_async()` in Python, a `std::future` in C++.

The tests in `tests/python/test_examples.py` run every file here and check what it does.
