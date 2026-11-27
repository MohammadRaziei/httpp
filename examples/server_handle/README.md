# server_handle

Mount a folder on a server you do not have yet and serve it over TLS, in one chain.

The page that explains it: [https://mohammadraziei.github.io/httpp/docs/server/server-handle/](https://mohammadraziei.github.io/httpp/docs/server/server-handle/)

It needs a folder `public` with some files in it, and a certificate and key `cert.pem` and `key.pem` (for a test: `openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 1 -subj /CN=localhost`).

## Run

```bash
python python/main.py       # Python
```

For C++, build `cpp/main.cpp` like any program that uses httpp (CMake and `httpp::httpp_core`, see the [Install page](https://mohammadraziei.github.io/httpp/docs/install/)) and run the program.

It listens on `127.0.0.1:8000` (HTTPS). In another terminal:

```bash
httpp curl -k https://127.0.0.1:8000/
```

## More files

- `mount_into` (Python and C++): mount a folder on a server you already have: `/static` is the folder, `/ping` is a route (`curl http://127.0.0.1:8000/ping`; this one is plain HTTP).
- `stop` (Python and C++): stop the server from one of its own handlers: `curl http://127.0.0.1:8000/quit` answers `bye` and the program ends two seconds later at the most (plain HTTP).

The tests in `tests/python/test_examples.py` run every file here and check what it does.
