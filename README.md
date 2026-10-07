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
  - [Serving: routes](#serving-routes) · [Handler reference](#handler-reference) · [Streaming and files](#streaming-and-files) · [Settings](#settings) · [Hooks and error handling](#hooks-and-error-handling) · [Running the server](#running-the-server)
  - [Client](#client) · [Errors](#errors) · [Request builder](#request-builder) · [Sessions and async](#sessions-and-async) · [Downloads](#downloads) · [Progress bars and URLs](#progress-bars-and-urls)
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
- **Hooks, streaming and files** on the server: `before_request` / `after_request`, error and exception
  handlers, generators as streamed bodies, files sent from disk, cookies and redirects.
- **A client with a real error model**: `res.json()`, `res.error`, `raise_for_status()`, typed
  exceptions, sessions, an `AsyncClient`, timeouts, redirects, TLS verification.
- **Downloads** that can resume, with a tqdm-style progress bar (drawn on stderr, so piped output
  stays clean). The features above exist in both C++ and Python, with similar APIs.
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
| `(body, status, headers)` | same, with extra headers: a dict, or a list of `(name, value)` pairs when a name repeats, e.g. several `Set-Cookie` (a `Content-Type` you set wins) |
| a generator / async generator | `200`, sent in chunks as it produces `str` or `bytes` pieces (see [Streaming and files](#streaming-and-files)) |
| `httpp.file(path)` | `200`, the file streamed from disk, `Content-Type` guessed from its extension |

The `request` object:

| Attribute | Meaning |
|---|---|
| `method`, `path` | HTTP method and path |
| `body` / `text` / `json()` | raw bytes, decoded text, parsed JSON |
| `headers` | `dict`, names lower-cased (`request.headers["x-token"]`) |
| `query` | `dict` of `?a=1` parameters |
| `path_params` | `dict` of captured path parameters (all `str`) |
| `cookies` | `dict` parsed from the `Cookie` header (first value wins) |

For repeated headers or query names the first value wins.

Error behavior:

- Return a status yourself for expected failures: `return {"error": "not found"}, 404`.
- A path parameter annotated `int`/`float` that does not parse answers **422** automatically.
- An uncaught exception answers a generic **500**; the traceback goes to stderr, never to the client.
  `@app.exception_handler(...)` and `@app.error_handler(...)` let you shape these answers (see
  [Hooks and error handling](#hooks-and-error-handling)).
- A response header whose name or value contains a line break is refused (a 500), never sent.

### Streaming and files

```python
import asyncio, httpp

@app.get("/events")
async def events():
    async def pieces():
        for n in range(3):
            await asyncio.sleep(1)
            yield f"data: {n}\n\n"            # str or bytes
    return pieces(), 200, {"Content-Type": "text/event-stream"}

@app.get("/report")
def report():
    return httpp.file("/srv/reports/latest.pdf")      # streamed from disk, length known

@app.get("/old")
def old():
    return httpp.redirect("/new", 301)                # default status 302

@app.get("/login")
def login():
    return "welcome", 200, [httpp.set_cookie("session", token, max_age=3600, same_site="lax"),
                            httpp.set_cookie("theme", "dark", http_only=False)]
```

- A **generator** (or **async generator**, in a plain or an `async def` route) is sent chunked, one
  piece at a time, so the whole body never sits in memory. Without a `Content-Type` of yours it is
  `text/plain; charset=utf-8`. A generator that raises midway **drops the connection**: the client sees a
  truncated body rather than a clean end, and the traceback goes to stderr. If the client hangs up, the
  generator is released.
- `httpp.file(path)` takes an optional `content_type`. A missing or unreadable file answers **404**.
  `path` is trusted: never build it from user input (path traversal); use `serve_directory` for that.
- `httpp.set_cookie` defaults to `Path=/; HttpOnly`, like the C++ helper. A name or value that is not
  cookie-safe raises `ValueError` (percent-encode spaces, quotes, commas and semicolons yourself).

### Settings

```python
app = Server(threads=8, read_timeout=10, max_body=5_000_000, handler_timeout=30)
```

| Argument | Meaning (leave it out to keep the default) |
|---|---|
| `threads`, `max_threads` | worker threads for plain `def` handlers: `threads` are kept, up to `max_threads` are used under load. Default: about one per core, at least 8, growing to 4x that |
| `read_timeout`, `write_timeout`, `keep_alive_timeout` | seconds to read a request, write a response, keep an idle connection (default 5 each) |
| `keep_alive_max` | requests served per connection (default 100) |
| `max_body` | largest request body in bytes, refused above it with a **413** (a client still sending a huge body may see the connection closed instead); `0` = no limit |
| `handler_timeout` | seconds an `async def` handler may take (default **60**, `None` = never): after that the coroutine is **cancelled** and the client gets a **504** |

`handler_timeout` only covers `async def` handlers: a plain `def` handler cannot be cancelled. The time
spent streaming the body afterwards is not counted. The same settings exist as methods
(`app.set_read_timeout(10)`, ...), each returning `app`, mirroring the C++ `httpp::server`.

### Hooks and error handling

```python
@app.on_startup
def starting():
    print("about to listen")

@app.before_request
def require_key(request):
    if request.headers.get("x-key") != "secret":
        return {"error": "forbidden"}, 403          # answers here; the route never runs

@app.after_request
def stamp(request, response):
    response.set_header("X-Served-By", "httpp")      # status and headers only

@app.error_handler(404)
def not_found(request):
    return {"error": "no such page", "path": request.path}

@app.exception_handler(ValueError)
def bad_value(request, exc):
    return {"error": str(exc)}, 400
```

- `before_request` returns `None` to carry on, or anything a route may return to answer right there.
  `after_request` gets a `ResponseHead` (`status`, `headers` as a list of pairs, plus `header()`,
  `set_header()`, `add_header()`); the body is not passed, because it may be a huge file or a stream.
- Both run for **every** request, static files and unknown paths included, and you can register several:
  they run in registration order. Hooks may be `async def`, but they hold their worker thread while they
  await. A hook that raises answers a **500**.
- `error_handler(404, ...)` dresses only errors **the server** makes itself (no such route, body too
  large, ...), never a response a route returned on purpose, and it keeps the original status unless
  you return another. A handler that raises leaves the plain error as it was.
- `exception_handler(Exc, ...)` runs when a route raises; the most specific class wins, the status
  defaults to 500, and an exception nobody handles is still the generic 500. A handler that raises
  itself is a generic 500 too.
- `on_startup` / `on_shutdown` (plain or `async def`) run around `listen()`: startup before the server
  listens, shutdown after it stopped, in reverse order. A failing startup hook stops the launch; a failing
  shutdown hook is logged and the others still run.

### Running the server

`listen()` is the launcher: `python app.py` is all it takes.

```python
if __name__ == "__main__":
    app.listen("0.0.0.0", 8000)
```

- It blocks until `stop()` is called or you press **Ctrl+C**.
- It raises `OSError` if the address cannot be bound (for example, the port is already taken).
  Two httpp servers cannot share a port.
- Plain `def` handlers run on the server's thread pool (see [Settings](#settings)), so a handler that
  blocks holds its thread for as long as it blocks.
- `async def` handlers run on **one shared asyncio event loop** (a background thread, started on first
  use) and give their worker thread back as soon as they start. While they `await`, no thread is held,
  so far more requests can be in flight than there are threads: on a 1-core test machine, 400
  concurrent requests that each `await asyncio.sleep(0.5)` all finish in about 0.7 s. Because every
  request shares the same loop, loop-bound objects (an `aiohttp.ClientSession`, an async DB pool) can be
  created once and reused. A blocking call inside an `async def` handler blocks **every** async route,
  since they all share that loop.
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
import httpp

res = httpp.get("http://example.com/items", params={"page": 2}, timeout=10)
res = httpp.post("http://localhost:8000/items", json={"name": "apple"}, token="abc")

print(res.status, res.ok)                  # 200 True
print(res.json())                          # parsed body; res.text is str, res.content is bytes
print(res.header("content-type"))          # case-insensitive, or None
print(res.headers)                         # [(name, value), ...]
```

`httpp.get`, `head`, `options`, `post`, `put`, `patch`, `delete` and `httpp.request(method, url, ...)`
take the same options: `params`, `headers`, `data`, `json`, `form`, `content_type`, `timeout`,
`follow_redirects`, `auth=(user, password)`, `token`, `cookies`, `verify`, `ca_file`, `proxy=(host, port)`.

- Timeouts are whole seconds (rounded, minimum 1). Redirects are **not** followed unless you ask
  (`follow_redirects=True`); downloads, by contrast, do follow them.
- `https://` verifies the certificate by default; `verify=False` or `ca_file="ca.pem"` change that.
- `Client.fetch(url)` and `Client(host, port).get(path)` still work.

### Errors

A non-2xx status is a response, not an error, and nothing raises unless you ask:

```python
res = httpp.get(url, timeout=5)
if res.failed:                              # no HTTP response at all
    print(res.error, res.error_message)     # 'connection', 'timeout', 'tls', ...

data = httpp.get(url).raise_for_status().json()   # raises, or returns res so calls chain
```

`res.error` is `None` or one of `connection`, `timeout`, `tls`, `too_many_redirects`, `too_large`,
`invalid_url`, `unsupported_method`, `canceled`, `other`; `res.status` is `0` when there was no response.
`raise_for_status()` raises `httpp.HTTPError` for a 4xx/5xx (with `.response`) and, when nothing
arrived, `httpp.RequestError` or its subclass `Timeout`, `TLSError`, `TooManyRedirects`, `InvalidURL`
(also a `ValueError`). All derive from `httpp.Error`.

### Request builder

A fluent builder for one-off requests, with the common curl options (`-X`, `-H`, `-d`, `-u`, `-L`, `-k`, ...):

```python
from httpp import Request

res = (
    Request("http://example.com/api")
    .method("POST")
    .header("X-Token", "abc")
    .param("v", "2")
    .json({"a": 1})
    .bearer(token)
    .timeout(10)
    .follow_redirects()
    .run()
)
res = await Request(url).run_async()
```

Methods: `method`, `header`, `param` / `params`, `data`, `json`, `form`, `content_type`, `basic_auth`,
`bearer`, `cookie`, `timeout`, `follow_redirects`, `verify`, `ca_file`, `proxy`, `max_response_size`,
then `run()` or `run_async()`. Without `.content_type()`, `data` is sent as
`application/x-www-form-urlencoded`, like `curl -d`.

### Sessions and async

```python
client = httpp.Client("http://localhost:8000/api", headers={"X-Token": "abc"}, timeout=10)
client.get("/items", params={"page": 2}).json()
client.post("/items", json={"name": "apple"}).raise_for_status()

async with httpp.AsyncClient("http://localhost:8000", max_connections=16) as c:
    replies = await asyncio.gather(*(c.get(f"/items/{i}") for i in range(100)))
```

A `Client` keeps its connection alive between calls (no new TCP/TLS handshake) and its calls take turns.
`AsyncClient` is a pool of such clients, at most `max_connections`, each request waiting its turn.

**What "async" means here:** every blocking call runs on the event loop's default thread pool
(`Request.run_async()` and `Download.run_async()` too). The loop is never blocked, but this is *not*
non-blocking socket I/O, so expect tens of concurrent requests, not thousands. The C++ `async_client`
works the same way.

### Downloads

```python
from httpp import download, Download

result = download("http://example.com/big.zip", "big.zip")        # progress bar on stderr
result = Download(url, "big.zip").resume().disable_progress().run()
result = await Download(url, "big.zip").resume().run_async()      # doesn't block the event loop

if not result.ok:
    print(result.status, result.error)
```

| Builder method | Effect |
|---|---|
| `output(path)` | where to save (or pass it to `Download(url, path)`) |
| `enable_progress()` / `disable_progress()` | the progress bar (on by default) |
| `follow_redirects(False)` | treat a 3xx as the final answer; redirects are followed by default |
| `resume()` | `wget -c`: a complete `dest` is **skipped** (`result.skipped`, only a HEAD request is sent); otherwise bytes go to `dest.part`, continued with a `Range` request if it already has some, and renamed to `dest` only when complete, so `dest` is never left partial. A server that ignores `Range` gets a full restart; one that can't answer HEAD and Range degrades to a fresh download |
| `force()` | with `resume()`: ignore existing files and start from byte 0 |

`result` has `ok`, `status` (`0` when no response arrived), `error` and `skipped`. A failed download leaves
no file at `dest`. There is no timeout option on downloads yet, in C++ or Python.

### Progress bars and URLs

```python
from httpp import URL, progress

for i in progress.range(100, "working"):          # like tqdm.trange, drawn on stderr
    ...

bar = progress.bar(total, "copying")
bar.update(n)            # or bar.set_progress(position); bar.finish()

u = URL.parse("https://example.com:8443/a/b?x=1")
u.valid, u.scheme, u.host, u.port, u.path, u.query       # parsing never raises: check .valid
URL.encode_component("a b/é")                  # 'a%20b%2F%C3%A9'
URL.decode_component("a%20b+c")                # 'a b+c'  ("+" stays "+")
URL.build_query({"q": "x y", "n": 2})          # 'q=x%20y&n=2'
```

`URL` is the same parser the client uses; `build_query` takes a dict or a list of pairs
(values `str`, `bytes`, `int` or `float`).

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
| [`12_hooks_and_settings.py`](examples/python/12_hooks_and_settings.py) | settings, `before`/`after` hooks, error and exception handlers, startup/shutdown |
| [`13_streaming_and_files.py`](examples/python/13_streaming_and_files.py) | generators, `httpp.file`, redirects, cookies |
| [`14_async_client.py`](examples/python/14_async_client.py) | `AsyncClient`, awaitable downloads, the handler timeout (504) |
| [`15_progress_and_url.py`](examples/python/15_progress_and_url.py) | `progress.range` / `progress.bar` and `URL` |

The client examples (09 to 15) start their own local server, so they run offline.

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
builds should pass the first option. See [`src/third_party/README.md`](src/third_party/README.md#asio).

## Status

httpp is **alpha**: the API can still change between minor versions. Current limitations:

- The async client is backed by a thread pool, not non-blocking sockets (see
  [Sessions and async](#sessions-and-async)); downloads have no timeout option; the server's hooks hold
  a worker thread while they await; a plain `def` handler cannot be cancelled by `handler_timeout`.
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
