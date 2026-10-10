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
- **The server is event-driven** on every platform: all socket I/O (accepting, reading, keep-alive idle
  connections, timeouts, writing) runs on one Asio thread (epoll on Linux, kqueue on macOS, IOCP on
  Windows), and requests are parsed by llhttp. Idle connections cost no worker thread. A worker is used
  only while your handler (or a file/stream body piece) is actually running, never while waiting for the
  network. This also applies to async routes. Keep-alive works on every platform.
```

Most dependencies are vendored (`cpp-httplib`, unmodified; `llhttp`, unmodified) or git submodules
(`mbedtls`, `liburlparser`, `asio`). The full history of the `mbedtls` submodule is large; if you only need the source, clone
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
- Platforms: the connection layer is tested on Linux. Windows and macOS are untested by the author; CI
  on those platforms is the real check.
- There is no ASGI or WSGI interface (see above), so ASGI/WSGI applications such as FastAPI or Flask
  cannot be mounted on it yet.
- `cpp-httplib` (unmodified) now serves only the synchronous client; the server and the async engine are
  our own (Asio + llhttp). It is planned to retire it once the synchronous client is ours too.
- The libcurl compatibility layer covers a subset of `curl_easy_*`.
- No ASGI/WSGI interface, auto-reload or multi-process workers.

Issues and pull requests are welcome.

## License

[MIT](LICENSE.txt)
