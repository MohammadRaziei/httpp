# httpp

<div align="center">

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE.txt)
[![CMake 3.21+](https://img.shields.io/badge/CMake-3.21+-blue.svg)](https://cmake.org/)
[![C++17](https://img.shields.io/badge/C++-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Python 3.8+](https://img.shields.io/badge/Python-3.8+-blue.svg)](https://www.python.org/)

</div>

A lightweight HTTP **client + server** library for C++ and Python.
`pip install httpp` and you get: a Python package, a CLI, a compiled C++
library with headers and a CMake config — no system dependencies at all
(no `apt`/`choco`/`brew`, nothing outside what `pip` already gives you).

## Why

- **Client and server in one library** — most lightweight HTTP libraries
  only do one side.
- **Zero system dependencies** — every third-party piece is vendored and
  built from source.
- **One `pip install`, three ways in** — Python API, a CLI, and a real C++
  library (headers + `find_package(httpp)`) all come from the same wheel.
- **A migration path for existing `libcurl` code** — `httpp/curl_compat.h`
  gives you `curl_easy_*` so you can often switch with just a header swap.

## Install

```bash
pip install httpp
```

## Quick start

### Python

```python
from httpp import Client, Server, DownloadFile, CurlRequest

# client
res = Client.fetch("http://example.com/")
print(res.status, res.body)

# server — like `python -m http.server`, but embeddable in your own app
srv = Server()
srv.serve_directory("/", "./public")
srv.listen("0.0.0.0", 8000)

# download a file, with a tqdm-like progress bar
DownloadFile("http://example.com/big.zip").output("big.zip").run()

# a small curl-flavored request builder
res = CurlRequest("http://example.com/api").method("POST").data("a=1").run()
```

### CLI

```bash
httpp server .                              # serve the current directory
httpp download http://example.com/          # print body to stdout
httpp download http://example.com/f.zip -o f.zip   # save to a file, with a progress bar
httpp curl -X POST -H "X-Token: abc" -d "a=1" http://example.com/api
httpp install --user                        # put headers/lib/cmake config on your system
```

### C++

```cpp
#include <httpp.h>

auto res = httpp::client::fetch("http://example.com/");

httpp::server srv;
srv.get("/hello", [](const httpp::request&, httpp::response& res) {
    res.status = 200;
    res.body = "world";
});
srv.listen("0.0.0.0", 8000);

httpp::download_file("http://example.com/f.zip").output("f.zip").run();

auto res2 = httpp::curl::request("http://example.com/api")
                .method("POST")
                .header("X-Token", "abc")
                .data("a=1")
                .run();
```

### Migrating from libcurl

```c
#include <httpp/curl_compat.h>   /* was: #include <curl/curl.h> */

CURL *curl = curl_easy_init();
curl_easy_setopt(curl, CURLOPT_URL, "http://example.com/");
curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
CURLcode rc = curl_easy_perform(curl);
curl_easy_cleanup(curl);
```

Covers the common `curl_easy_*` subset (URL, method, headers, POST data,
write callback, timeout, follow-redirect, response code) — not a full
libcurl replacement. See the comment at the top of `curl_compat.h` for the
exact list.

### Using it from CMake, from a plain `pip install`

No system-wide install needed — point CMake at the package Python already
installed:

```cmake
execute_process(
    COMMAND python3 -c "import httpp; print(httpp.get_cmake_dir())"
    OUTPUT_VARIABLE httpp_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
find_package(httpp CONFIG REQUIRED)
target_link_libraries(app PRIVATE httpp::httpp_core)
```

Or, after `httpp install [--user]`, plain `find_package(httpp)` works with
no Python involved at all — see [`tests/cmake/consumer_project`](tests/cmake/consumer_project)
for a full working example.

## Build from source

```bash
git clone --recurse-submodules https://github.com/MohammadRaziei/httpp.git
cd httpp
pip install -r requirements-dev.txt
cmake -B build -DHTTPP_BUILD_TESTS=ON -DHTTPP_BUILD_PYTHON=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure   # C++, Python, and CMake-integration tests
```

## Status

- [x] `httpp::client` — GET requests; `.fetch(url)` parses a full URL in one call
- [x] `httpp::server` — route handlers, static directory serving
- [x] `httpp::curl` — a small curl-flavored fluent request builder (method/headers/data)
- [x] `httpp::download_file` — fluent download builder with a terminal progress bar
      (sync `.run()` and async `.run_async()`)
- [x] `httpp::progress` — `bar` (tqdm-like) and `range` (trange-like)
- [x] `httpp/curl_compat.h` — a `curl_easy_*` C API subset for migrating
      existing libcurl code, built directly on `httpp::curl::request`
- [x] CLI — `server`, `download`, `curl`, `install`/`uninstall`
- [x] Python bindings for `Client`, `Server`, `DownloadFile`, `CurlRequest`
- [ ] HTTPS
- [ ] Route handlers from Python (directory serving works today; custom
      routes are C++-only for now)

## License

MIT
