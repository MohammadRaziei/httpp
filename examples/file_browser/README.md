# file_browser

A file browser for a folder, with a page, a JSON listing and raw downloads, mounted under `/files`.

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/server/file-browser/](https://mohammadraziei.github.io/httpp/docs/server/file-browser/)

It needs a folder `shared` with some files in it, next to where you run it.

## Run

```bash
python python/main.py       # Python
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

It listens on `127.0.0.1:8000`. In another terminal:

```bash
# open http://127.0.0.1:8000/files/ in a browser, or:
curl "http://127.0.0.1:8000/files/api/list?path=/"
```

The tests in `tests/python/test_examples.py` run every file here and check what it does.
