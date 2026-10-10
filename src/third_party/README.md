# third_party

Vendored dependencies. Each is either a committed file (`cpp-httplib`,
`indicators`) or a git submodule built from source (`mbedtls`,
`liburlparser`, `asio`) — never a system package.

## cpp-httplib

`cpp-httplib/httplib.h` is the **unmodified** upstream file, version 0.54.1, MIT. Only the synchronous
client (`src/core/client.cpp`) uses it; the server and the async client do not. To restore or bump it:

```sh
wget -O src/third_party/cpp-httplib/httplib.h \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.54.1/httplib.h
```

It is to be retired once the synchronous client is also ours.

## asio

A git submodule (`asio/`, <https://github.com/chriskohlhoff/asio>) pinned to the tag
`asio-1-34-2`. It is header-only and used unmodified, for the event loop in
`src/core/async_server.cpp` and `src/core/async_http.cpp` (epoll on Linux, kqueue on macOS,
IOCP on Windows), built with `ASIO_STANDALONE`. Boost Software License 1.0.

The top-level `CMakeLists.txt` looks for Asio in this order:

1. `-DHTTPP_ASIO_INCLUDE_DIR=<directory containing asio.hpp>` (offline builds, packagers)
2. this submodule: `src/third_party/asio/asio/include` (or a plain copy in `src/third_party/asio/include`)
3. otherwise it downloads Asio 1.34.2 at configure time (about 3 MB, pinned by SHA-256), so a source
   tree without submodules, such as an sdist, still builds.

To move to another Asio version, check out its tag in the submodule and commit the new pointer (also
update `HTTPP_ASIO_VERSION` / `HTTPP_ASIO_SHA256` in `CMakeLists.txt`, which only matter for step 3).

## indicators (not wired up yet)

`indicators/indicators.hpp` — a single-header terminal progress-bar/spinner
library (github.com/p-ranav/indicators). Vendored for later use (e.g. a
progress bar for `httpp download`), not currently included or built by
anything.

## Local patches

- **liburlparser/CMakeLists.txt**: two `if(DEFINED SKBUILD)` checks were
  narrowed to `if(DEFINED SKBUILD AND SKBUILD_PROJECT_NAME STREQUAL PROJECT_NAME)`.
  Upstream assumes it's always the top-level scikit-build-core package, so
  as soon as `SKBUILD` is defined it tries to build its own nanobind Python
  bindings (and otherwise `RETURN()`s before ever defining the `url::base`
  target). That assumption breaks when liburlparser is vendored as a
  subdirectory of another scikit-build-core project (httpp): `SKBUILD` is
  defined for *httpp's* install, not liburlparser's, so the guard now also
  checks that liburlparser is genuinely the package being installed.
