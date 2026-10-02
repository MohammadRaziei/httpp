# httpp

<div align="center">

**A lightweight HTTP client and server for Python and C++ — one `pip install`, no system dependencies.**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE.txt)
[![Python 3.8+](https://img.shields.io/badge/Python-3.8+-blue.svg)](https://www.python.org/)
[![C++17](https://img.shields.io/badge/C++-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CMake 3.21+](https://img.shields.io/badge/CMake-3.21+-blue.svg)](https://cmake.org/)
[![Status: alpha](https://img.shields.io/badge/status-alpha-orange.svg)](#status)

</div>

```python
from httpp import Server

app = Server()

@app.get("/hello/{name}")
def hello(name):
    return f"hello {name}"

app.listen("127.0.0.1", 8000)   # that's it — no separate server process needed
```

`pip install httpp` gives you a Python package, a command-line tool, and a compiled C++ library with
headers and a CMake config. Every third-party component is built from source, so nothing has to be
installed system-wide (no `apt`, `brew` or `choco`).

## Contents

- [Features](#features)
- [Install](#install)
- [Python](#python)
  - [Serving: routes](#serving-routes) · [Handler reference](#handler-reference) · [Running the server](#running-the-server)
  - [Client](#client) · [Request builder](#request-builder) · [Downloads](#downloads)
- [Command line](#command-line)
- [C++](#c)
- [Migrating from libcurl](#migrating-from-libcurl)
- [Using httpp from CMake](#using-httpp-from-cmake)
- [Examples](#examples)
- [Build from source](#build-from-source)
- [Status](#status)
- [License](#license)

## Features

- **Client and server in one library.** Most lightweight HTTP libraries only do one side.
- **Decorator-style routing in Python**, in the spirit of Flask and FastAPI: path parameters, typed
  conversion, JSON responses, `async def` handlers.
- **Built-in server.** No ASGI/WSGI layer and no separate process to launch.
- **Real async.** `async def` handlers run on one shared asyncio event loop and release their
  worker thread while they `await`. Idle connections wait in an event loop too (epoll, kqueue or IOCP,
  via [Asio](https://think-async.com/Asio/)), so thousands of slow or idle requests cost
  almost no threads.
- **Python, C++ and a CLI from the same wheel.** One binary core, three front ends.
- **HTTP and HTTPS** on the client (mbedtls, vendored).
- **Downloads** with a tqdm-style progress bar (drawn on stderr, so piped output stays clean).
- **A migration path for `libcurl` code**: `httpp/curl_compat.h` provides a `curl_easy_*` subset.
- **No system dependencies.** Nothing to install system-wide; the only build-time download is the
  header-only Asio (about 3 MB, pinned by hash), which can be supplied locally instead.

## Install

```bash
pip install httpp
```

Requires Python 3.8 or newer. Building the C++ side on its own needs CMake 3.21+ and a C++17 compiler
(see [Build from source](#build-from-source)).

## Python

### Serving: routes

Register handlers with decorators, then call `listen()`:

```python
import asyncio
from httpp import Server

app = Server()

@app.get("/hello/{name}")              # "/hello/<name>" is accepted too
def hello(name):
    return f"hello {name}"

@app.get("/square/{n}")
def square(n: int):                    # annotate as int/float to get a converted value
    return {"n": n, "square": n * n}   # dict/list -> JSON

@app.post("/items")
def create_item(request):              # ask for `request` by name
    return {"got": request.json()}, 201, {"Location": "/items/1"}

@app.get("/slow")
async def slow():                      # async handlers: no thread is held while awaiting
    await asyncio.sleep(1)
    return "done"

app.listen("127.0.0.1", 8000)
```

All of `get`, `post`, `put`, `patch` and `delete` are available, and `route()` takes a list of methods
(`@app.route("/thing", methods=["PUT", "PATCH"])`). Register routes **before** calling `listen()`.

Static files can be served next to your routes:

```python
app.serve_directory("/static", "./public")
```

### Handler reference

A handler may declare any of its path parameters by name, plus `request`. It returns one of:

| Return value | Response |
|---|---|
| `str` | `200`, `text/plain; charset=utf-8` |
| `bytes` | `200`, `application/octet-stream` |
| `dict` / `list` | `200`, `application/json` |
| `None` | `200`, empty body |
| `(body, status)` | same, with your status code |
| `(body, status, headers)` | same, with extra headers (a `Content-Type` you set wins) |

The `request` object:

| Attribute | Meaning |
|---|---|
| `method`, `path` | HTTP method and path |
| `body` / `text` / `json()` | raw bytes, decoded text, parsed JSON |
| `headers` | `dict`, names lower-cased (`request.headers["x-token"]`) |
| `query` | `dict` of `?a=1` parameters |
| `path_params` | `dict` of captured path parameters (all `str`) |

For repeated headers or query names the first value wins.

Error behavior:

- Return a status yourself for expected failures: `return {"error": "not found"}, 404`.
- A path parameter annotated `int`/`float` that does not parse answers **422** automatically.
- An uncaught exception answers a generic **500**; the traceback goes to stderr, never to the client.

### Running the server

`listen()` is the launcher: `python app.py` is all it takes.

```python
if __name__ == "__main__":
    app.listen("0.0.0.0", 8000)
```

- It blocks until `stop()` is called or you press **Ctrl+C**.
- It raises `OSError` if the address cannot be bound (for example, the port is already taken).
  Two httpp servers cannot share a port.
- Plain `def` handlers run on the server's thread pool (at most a few dozen threads, depending on the
  machine), so a handler that blocks holds its thread for as long as it blocks.
- `async def` handlers run on **one shared asyncio event loop** (a background thread, started on first
  use) and give their worker thread back as soon as they start. While they `await`, no thread is held,
  so far more requests can be in flight than there are threads: on a 1-core test machine, 400
  concurrent requests that each `await asyncio.sleep(0.5)` all finish in about 0.7 s. Because every
  request shares the same loop, loop-bound objects (an `aiohttp.ClientSession`, an async DB pool) can be
  created once and reused.
- **Connections are event-driven** on every platform: idle connections, including HTTP keep-alive
  ones, wait in an Asio event loop (epoll on Linux, kqueue on macOS, IOCP on Windows) on the listening
  thread and cost no worker thread. A worker is used only while a request is actually being read and
  answered, then the connection goes back to the loop. On Linux, 3000 idle connections were held by that
  one thread and a request still took 0.2 ms, the same as with none. This also applies to async routes.
- Keep-alive after a response needs the loop to take the socket back, which Windows supports from 8.1 on.
  On older Windows (and under Wine) the loop notices this and answers `Connection: close`: requests are
  still served and idle connections still cost no thread, there is just no connection reuse.
- CPU-bound handlers are still limited by the GIL, async or not.
- A request that has only partly arrived holds a worker thread until it completes or times out.

There is no ASGI/WSGI interface, so servers such as uvicorn or gunicorn do not apply, and there is no
auto-reload or multi-process mode.

### Client

```python
from httpp import Client

res = Client.fetch("http://example.com/")          # full URL in one call
print(res.status, res.ok)                          # 200 True
print(res.body)                                    # body as str
print(res.header("content-type"))                  # case-insensitive, or None
print(res.headers)                                 # [(name, value), ...]

client = Client("example.com", 80)                 # or host + port
print(client.get("/index.html").status)
```

A non-2xx status is not an exception; check `res.ok` or `res.status`.

### Request builder

A small, curl-flavored fluent builder for the common cases (`-X`, `-H`, `-d`):

```python
from httpp import Request

res = (
    Request("http://example.com/api")
    .method("POST")
    .header("X-Token", "abc")
    .content_type("application/json")
    .data('{"a": 1}')
    .run()
)
```

Without `.content_type()`, data is sent as `application/x-www-form-urlencoded`, like `curl -d`.

### Downloads

```python
from httpp import download, Download

result = download("http://example.com/big.zip", "big.zip")      # progress bar on stderr
result = download(url, "big.zip", show_progress=False)
result = Download(url).output("big.zip").disable_progress().run()

if not result.ok:
    print(result.status, result.error)
```

## Command line

```bash
httpp server .                                     # serve a directory, like `python -m http.server`
httpp server ./public --host 127.0.0.1 --port 9000

httpp download http://example.com/                 # print the body to stdout
httpp download http://example.com/f.zip -o f.zip   # save to a file, with a progress bar

httpp curl -X POST -H "X-Token: abc" -d "a=1" http://example.com/api

httpp install --user                               # install headers, library and CMake config
httpp path --include-dir                           # print include/lib/cmake locations
```

## C++

The same server, client and download builder are available natively. Include the umbrella header:

```cpp
#include <httpp.h>
#include <chrono>
#include <thread>

// Server
httpp::server srv;

srv.get("/hello", [](const httpp::request&, httpp::response& res) {
    res.body = "world";                               // status defaults to 200
});

srv.post("/users/:id", [](const httpp::request& req, httpp::response& res) {
    // req.method, req.path, req.body
    // req.headers, req.query, req.path_params: vectors of (name, value) pairs
    res.status = 201;
    res.headers.emplace_back("Content-Type", "application/json");
    res.body = R"({"ok": true})";
});

// Async: the handler gets a `responder` and may return at once, releasing the worker
// thread; call send() later, from any thread (an event loop, a timer, a callback).
srv.route_async("GET", "/slow", [](const httpp::request&, httpp::responder r) {
    std::thread([r] {                                 // stand-in for "wait for something"
        std::this_thread::sleep_for(std::chrono::seconds(1));
        httpp::response res;
        res.status = 200;
        res.body = "done";
        r.send(res);                                  // thread-safe; first call wins
    }).detach();
});

srv.serve_directory("/static", "./public");
srv.listen("0.0.0.0", 8000);                          // false if it could not bind; true after stop()

// Client
auto res = httpp::client::fetch("http://example.com/");

auto res2 = httpp::client::request("http://example.com/api")
                .method("POST")
                .header("X-Token", "abc")
                .data("a=1")
                .timeout(10)
                .follow_redirects()
                .run();

// Download
auto result = httpp::download("http://example.com/f.zip").output("f.zip").resume().run();
```

`get`, `post`, `put`, `patch` and `del` (`delete` is a C++ keyword) are shorthands for the general
`route(method, path, handler)`, which accepts `GET`, `POST`, `PUT`, `PATCH`, `DELETE` and `OPTIONS`
(case-insensitive) and throws `std::invalid_argument` for anything else. `:name` segments in the path are captured into
`request::path_params`.

`route_async()` is the non-blocking variant: its handler receives a `responder` instead of a `response&`.
An exception thrown by it is answered with a plain 500; dropping every copy of the `responder` without
calling `send()` closes the connection unanswered; and if the server is destroyed first, pending
connections are closed and a later `send()` does nothing.

The Python decorators are a thin layer on top of these same `route()` / `route_async()` calls.

## Migrating from libcurl

Switch the header and keep most existing `curl_easy_*` code:

```c
#include <httpp/curl_compat.h>   /* was: #include <curl/curl.h> */

CURL *curl = curl_easy_init();
curl_easy_setopt(curl, CURLOPT_URL, "http://example.com/");
curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
CURLcode rc = curl_easy_perform(curl);
curl_easy_cleanup(curl);
```

It covers the common subset (URL, method, headers, POST data, write and header callbacks, timeout,
redirect following, response code, content type). It is not a full libcurl replacement; the comment at
the top of `curl_compat.h` lists exactly what is supported.

## Using httpp from CMake

No system-wide install is needed. Point CMake at the package that `pip` already installed:

```cmake
execute_process(
    COMMAND python3 -c "import httpp; print(httpp.get_cmake_dir())"
    OUTPUT_VARIABLE httpp_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
find_package(httpp CONFIG REQUIRED)
target_link_libraries(app PRIVATE httpp::httpp_core)
```

Alternatively, run `httpp install [--user]` once and a plain `find_package(httpp)` works with no Python
involved. A complete consumer project lives in [`tests/cmake/consumer_project`](tests/cmake/consumer_project).

## Examples

Standalone, runnable scripts, one feature each, in [`examples/python/`](examples/python). They assume
only `pip install httpp`.

| Example | Shows |
|---|---|
| [`01_hello_server.py`](examples/python/01_hello_server.py) | the smallest server |
| [`02_path_params.py`](examples/python/02_path_params.py) | `{name}` / `<name>`, `int` conversion, 422 |
| [`03_request_object.py`](examples/python/03_request_object.py) | query, headers, JSON body |
| [`04_responses.py`](examples/python/04_responses.py) | text, bytes, JSON, HTML, status, headers |
| [`05_http_methods.py`](examples/python/05_http_methods.py) | an in-memory CRUD API |
| [`06_async_handlers.py`](examples/python/06_async_handlers.py) | `async def` handlers |
| [`07_serve_directory.py`](examples/python/07_serve_directory.py) | static files next to routes |
| [`08_error_handling.py`](examples/python/08_error_handling.py) | 404, 500, port already in use |
| [`09_client.py`](examples/python/09_client.py) | `Client.get` and `Client.fetch` |
| [`10_request_builder.py`](examples/python/10_request_builder.py) | method, headers, body |
| [`11_download.py`](examples/python/11_download.py) | downloads and progress |

The client examples (09 to 11) start their own local server, so they run offline.

## Build from source

```bash
git clone --recurse-submodules https://github.com/MohammadRaziei/httpp.git
cd httpp
pip install -r requirements-dev.txt
cmake -B build -DHTTPP_BUILD_TESTS=ON -DHTTPP_BUILD_PYTHON=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure   # C++, Python and CMake-integration tests
```

Most dependencies are vendored (`cpp-httplib`, which carries two patches) or git submodules (`mbedtls`,
`liburlparser`). The full history of the `mbedtls` submodule is large; if you only need the source, clone
with `git clone --depth 1 --recurse-submodules --shallow-submodules`.

[Asio](https://think-async.com/Asio/) (header-only, used unmodified) is found in this order:
`-DHTTPP_ASIO_INCLUDE_DIR=<dir containing asio.hpp>`, then a copy in `src/third_party/asio` (for example a
git submodule), then it is downloaded at configure time (about 3 MB, pinned to 1.34.2 by SHA-256). Offline
builds should pass the first option. See [`src/third_party/asio/README.md`](src/third_party/asio/README.md).

## Status

httpp is **alpha**: the API can still change between minor versions. Current limitations:

- The Python `Request` builder supports method, headers, body and content type; timeouts, redirect
  control, resumable downloads and async downloads are C++-only for now.
- A request that has only partly arrived (a slow client still sending its headers or body) holds a
  worker thread until it completes or times out; only *idle* connections are free. HTTP/1.1 only; the
  server does not speak TLS.
- Platforms: the connection layer is tested on Linux (including under ThreadSanitizer). It is compiled
  for Windows with mingw-w64 and its Windows code path was exercised under Wine, which only covers the
  no-keep-alive fallback; macOS is untested by the author. CI on Windows and macOS is the real check.
- There is no ASGI or WSGI interface (see above), so ASGI/WSGI applications such as FastAPI or Flask
  cannot be mounted on it yet.
- The vendored `cpp-httplib` carries two small patches (deferred responses, and hooks for the external
  Asio event loop), which make the above possible; see
  [`HTTPP_PATCHES.md`](src/third_party/cpp-httplib/HTTPP_PATCHES.md).
- The libcurl compatibility layer covers a subset of `curl_easy_*`.
- No ASGI/WSGI interface, auto-reload or multi-process workers.

Issues and pull requests are welcome.

## License

[MIT](LICENSE.txt)
