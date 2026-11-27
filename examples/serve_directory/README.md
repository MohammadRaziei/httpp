# serve_directory

Serves a folder under a URL prefix: `/static/notes.txt` is `public/notes.txt`.

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/server/serve-directory/](https://mohammadraziei.github.io/httpp/docs/server/serve-directory/)

It needs a folder `public` with some files in it, next to where you run it.

## Run

```bash
python python/main.py       # Python
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

It listens on `127.0.0.1:8000`. In another terminal:

```bash
httpp curl http://127.0.0.1:8000/static/notes.txt
```

The tests in `tests/python/test_examples.py` run every file here and check what it does.
