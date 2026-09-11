# httpp

A lightweight HTTP client + server library for C++ and Python — install
with `pip`, no system dependencies (no `apt`/`choco`/`brew` required), works
as both a Python package and a C++ library via CMake.

## Install

```bash
pip install httpp
```

## Quick start

**Python**

```python
from httpp import Client, Server

# client
res = Client.fetch("http://example.com/")
print(res.status, res.body)

# server — like `python -m http.server`, but embeddable in your own app
srv = Server()
srv.serve_directory("/", "./public")
srv.listen("0.0.0.0", 8000)
```

**CLI**

```bash
httpp server .              # serve the current directory, like python -m http.server
httpp download http://example.com/
```

**C++**

```cpp
#include <httpp.h>

auto res = httpp::client::fetch("http://example.com/");

httpp::server srv;
srv.serve_directory("/", "./public");
srv.listen("0.0.0.0", 8000);
```

```cmake
find_package(httpp CONFIG REQUIRED)
target_link_libraries(app PRIVATE httpp::httpp)
```

## Status

- [x] Client — GET requests, `Client.fetch(url)` from a full URL
- [x] Server — route handlers, static directory serving
- [x] CLI — `httpp server`, `httpp download`
- [ ] HTTPS
- [ ] Route handlers from Python (directory serving works today)

## License

MIT
