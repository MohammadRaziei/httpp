# send_file

Streams a file from disk as a download.

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/server/send-file/](https://mohammadraziei.github.io/httpp/docs/server/send-file/)

It needs a file `report.txt` in the folder you run it from.

## Run

```bash
python python/main.py       # Python
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

It listens on `127.0.0.1:8000`. In another terminal:

```bash
httpp curl -OJ http://127.0.0.1:8000/report
```

The tests in `tests/python/test_examples.py` run every file here and check what it does.
