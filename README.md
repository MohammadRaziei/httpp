# httpp

A lightweight, **zero-system-dependency** HTTP client + server library for
C++ and Python — a replacement for libcurl for both making requests *and*
running a server, not a reimplementation of libcurl itself.

**Hard rule:** no system libraries (nothing that needs `apt`/`choco`/`brew`
installed for headers or linking). Everything third-party is vendored:
`cpp-httplib` is a committed header file, `mbedtls` is a git submodule —
both built from source. A clean checkout only needs a C++ compiler + CMake
(+ Python/Cython for the bindings).

## What it gives you

- **`httpp::client` / `httpp.Client`** — make HTTP(S) requests.
- **`httpp::server` / `httpp.Server`** — register routes or serve a
  directory (static file serving), the way you'd use `python -m http.server`
  but embeddable in your own app, from C++ or Python.
- **CLI** (`httpp server DIR`, analogous to `python -m http.server`;
  `httpp download URL`, a tiny libcurl-CLI-style helper).
- All of the above without ever exposing `httplib.h`/mbedtls to consumers:
  the public headers (`include/httpp.h` and `include/httpp/*.hpp`) use PIMPL,
  and the compiled core (`httpp_core` / `httpp_cy.*.so`) is built with hidden
  visibility so vendored symbols never leak into the public ABI.

## Status (TDD log)

Built strictly test-first, C++ and Python together: a failing test before
any implementation, smallest change to turn it green, repeat.

- [x] `httpp::url` — minimal http(s) URL view (scheme/host/port/path/query),
      implemented on top of the vendored **liburlparser** (git submodule,
      github.com/mohammadraziei/liburlparser) instead of a hand-rolled
      parser — real IPv4/IPv6/PSL-aware parsing, still hidden behind
      httpp's own public header (urlparser.h is only included from
      src/core/url.cpp).
- [x] `httpp::client` — GET requests, backed by vendored cpp-httplib;
      `client::fetch(full_url)` parses a whole URL and GETs it in one call
      (no manual URL parsing needed by callers — see the CLI's `download`).
- [x] `httpp::server` — route handlers, static directory serving, both
      "bind then listen" and "listen on a fixed port" flows.
- [x] Python bindings (`httpp.Client`, `httpp.Server`) over the same core.
- [x] CLI: `httpp server [DIR]`, `httpp download URL`.
- [ ] HTTPS via the vendored mbedtls submodule (currently HTTP-only).
- [ ] Route handlers from Python (`Server.get(path, callback)` — directory
      serving and plain GET/404 work today; Python route callbacks are next).
- [ ] `get_cmake_dir()` / `get_include_dir()` for downstream `find_package(httpp)`.

## Build & test (standalone, C++ + Python together)

```bash
pip install -r requirements-dev.txt
cmake -B build -DHTTPP_BUILD_TESTS=ON -DHTTPP_BUILD_PYTHON=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure   # runs both test_httpp_cpp and test_httpp_python
```

## Install / use as a Python package

```bash
pip install .
httpp server .              # like `python -m http.server`
httpp download http://example.com/
```

```python
from httpp import Client, Server

srv = Server()
srv.serve_directory("/", "./public")
srv.listen("0.0.0.0", 8000)
```

## Layout

```
include/
  httpp.h              umbrella public header (version macros + all public API)
  httpp/
    url.hpp, client.hpp, server.hpp, export.hpp
src/
  core/                httpp_core: client.cpp, server.cpp — the ONLY files
                        that #include <httplib.h>; hidden visibility so
                        those symbols never leak into the compiled library
  bindings/python/      Cython module (httpp_cy.pyx) + the httpp/ package
  third_party/
    cpp-httplib/        vendored (committed header, no system package)
    mbedtls/            git submodule (built from source, no system package)
    liburlparser/       git submodule (built from source, no system package;
                        small local patch — see third_party/README.md)
tests/
  cpp/                  C++ unit tests (test_*.cpp, GLOB'd), utest.h-based
  python/                pytest tests against the built Cython module
  utest/                 vendored single-header test framework
cmake/                  DynamicVersion / Startup / Optimize (ctoon-style) +
                         FindCython / UseCython
version.py              version/tag manager, reads include/httpp.h
.github/workflows/       cmake.yml (build+test+coverage), wheels.yml
                         (sdist+cibuildwheel), orchestrator.yml (release)
```

Test folder structure mirrors github.com/mohammadraziei/ctoon (`tests/cpp` +
`tests/python` + `tests/utest`); the Cython wiring in
`src/bindings/python/CMakeLists.txt` (SKBUILD branch + standalone
`ExternalProject_Add(build_python ...)` branch) mirrors
github.com/mohammadraziei/pygixml.

## Naming convention

C++ identifiers follow the standard library's style: `snake_case` for both
types and functions (`httpp::url`, `httpp::client`, `client::get`, not
`Url`/`Client::Get`). The Python-facing classes (`Client`, `Server`) use
ordinary PEP 8 `PascalCase`, as is conventional there.
