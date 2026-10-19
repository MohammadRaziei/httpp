# ADR-0003: Network engine = Asio + llhttp + Mbed TLS; cpp-httplib only for the sync client

- **Status:** accepted
- **Decided by:** the project owner
- **Source:** `Architecture.md` sections 3 and 6, `src/third_party/README.md`

## Context

A parser, an event loop and TLS must be chosen, with no system dependencies and a small wheel.

## Decision

| Role | Choice | How it is vendored |
|---|---|---|
| HTTP/1.1 parser (server and client) | llhttp | unmodified in `src/third_party/llhttp` |
| Event loop and I/O | Asio standalone | git submodule |
| TLS (client) | Mbed TLS | git submodule |
| URL | liburlparser | git submodule |
| Internal logging | AixLog v1.5.0 | one file, one marked patch |
| **Sync** client, `curl.cpp`, `download.cpp` | cpp-httplib v0.54.1 | `httplib.h` **untouched** |

The async server and client are not built on cpp-httplib. cpp-httplib is meant to be retired once the sync client is ours too.
The only AixLog patch makes sure it does not swap `std::clog` for the whole process (a library must not do that).

## Rejected alternatives

OpenSSL (heavy), libuv (an external dependency), h11 (Python, slow). The speed difference against uvicorn does not come from the parser,
because `httptools` is also built on llhttp.

## Consequences

- We never patch `httplib.h`; every vendor change must be recorded in `src/third_party/README.md`.
- The server has **no TLS** yet. Only the client speaks HTTPS.
- HTTP/2, HTTP/3, gRPC and SOCKS proxy are not goals.

## Related

ADR-0004, ADR-0010
