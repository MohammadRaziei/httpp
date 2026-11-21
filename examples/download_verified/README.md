# download_verified

Downloads a URL to a file and checks its SHA-256 while it arrives.

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/client/download/](https://mohammadraziei.github.io/httpp/docs/client/download/)

It talks to the server of [`get_route`](../get_route/), so start that one first (it listens on `127.0.0.1:8000`).

## Run

```bash
python python/main.py       # Python
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

The code is in `python/main.py` and `cpp/main.cpp`; the tests in `tests/python/test_examples.py` run both and check what they do.
