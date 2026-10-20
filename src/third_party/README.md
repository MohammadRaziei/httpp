# third_party

Vendored dependencies. Each is either a committed file (`cpp-httplib`,
`indicators`, `llhttp`, `stb_inflate`, `stb_deflate`) or a git submodule built from source (`mbedtls`,
`liburlparser`, `asio`), never a system package.

## llhttp

`llhttp/` holds the four generated C files of the upstream release `release/v9.4.3` (MIT), **unmodified**: the
HTTP/1.x parser of the async server and client. The checksums are in `llhttp/VENDORED.md`, together with the upgrade
steps; the files were also compared byte for byte with the upstream tag. It is built as its own C static target
(`httpp_llhttp` in the top `CMakeLists.txt`) so the generated code does not get httpp's warning flags.

## cpp-httplib

`cpp-httplib/httplib.h` is the **unmodified** upstream file, version 0.54.1, MIT. Only the synchronous
client (`src/core/client.cpp`) uses it; the server and the async client do not. To restore or bump it:

```sh
wget -O src/third_party/cpp-httplib/httplib.h \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.54.1/httplib.h
```

It is to be retired once the synchronous client is also ours.

## aixlog

`aixlog/aixlog.hpp` is the single-header AixLog v1.5.0 (<https://github.com/badaix/aixlog>, MIT), the logging
back end. It carries **one patch**, marked `[httpp patch]` in the file: upstream's `Log` constructor points
`std::clog` at itself for the whole process, which a library must not do, so it writes through a private
stream instead (the `LOG(...)` macros, which write to `std::clog`, therefore no longer reach the sinks;
httpp does not use them). Only `src/core/log.cpp` includes the header, because it defines global macros
(`LOG`, `TAG`, `ERROR`, `DEBUG`, ...). To upgrade: take the new upstream file and redo that one change.

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

`indicators/indicators.hpp` - a single-header terminal progress-bar/spinner
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

## swagger-ui

`swagger-ui/` holds the three Swagger UI 5.33.2 files httpp serves at `/docs` (Apache-2.0, upstream's licence files included). They are
committed files that are **compiled into libhttpp at CMake configure time** (`cmake/EmbedFile.cmake`), not shipped; see
`swagger-ui/VENDORED.md`.

## stb_inflate

`stb_inflate/stb_inflate.h` is the zlib/deflate **decoder** of Sean Barrett's `stb_image.h` v2.30
(<https://github.com/nothings/stb>, MIT or public domain), taken through libstb
(<https://github.com/MohammadRaziei/libstb>) and cut out of the 283 KB image header, so only the decoder (about 500 lines)
is vendored. The text between the `BEGIN verbatim` and `END verbatim` markers is **byte for byte** lines 4081 to 4594 of
`stb_image.h`; around it is a small shim (the typedefs and macros the decoder needs, an error-reason hook, and an output
ceiling hook that lets a caller refuse buffer growth past a limit). Only `src/core/inflate.cpp` includes it, because every
function is `static`. It exists for gzip and deflate response bodies in the client: no system zlib is needed.
The compressor is `stb_deflate` below. To upgrade: copy the same section from a new stb_image.h and
re-run `tests/cpp/test_inflate.cpp`.

## stb_deflate

`stb_deflate/stb_deflate.h` is the zlib/deflate **compressor** of libstb (<https://github.com/MohammadRaziei/libstb>,
`stb_image_write.h`): stb's `stbi_zlib_compress` rewritten with 4-byte hash-chain LZ77, lazy matching and a per-block choice of
dynamic, fixed or stored Huffman blocks (upstream stb only writes fixed blocks). The text between the `BEGIN verbatim` and
`END verbatim` markers is **byte for byte** lines 828 to 1284 of libstb's file, with a small shim around it. Only
`src/core/deflate.cpp` includes it. With `stb_inflate` it gives gzip/deflate in both directions (client and server), with
no system zlib. Its output is a plain zlib stream that the wrapper strips and re-wraps as zlib, gzip or raw deflate. To
upgrade: copy the same section from a new libstb and re-run `tests/cpp/test_deflate.cpp`.
