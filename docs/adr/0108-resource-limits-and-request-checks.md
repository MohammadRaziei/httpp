# ADR-0108: Resource limits and request checks from the 0.9.0 security review

- **Status:** implemented in C++ and Python (built and tested on Linux, CPython 3.14: the C++ suites, and the CLI, server, forms, file browser, middleware, client and TLS Python tests); not run on Windows or macOS
- **Date:** 2026-11-22
- **Decided by:** the project owner asked for the usual standard limits as defaults and said not to keep compatibility with earlier behavior; the choices of the numbers are the implementer's
- **Source:** `src/core/form.cpp`, `src/core/internal/urlencoded.hpp`, `src/core/server.cpp`, `src/core/async_server.cpp`, `src/core/file_browser.cpp`, `src/core/tls_session.cpp`, `src/core/async_http.cpp`, `src/core/internal/http_exchange.hpp`, `src/cli/httpp_cli.cpp`
- **Follows:** ADR-0095 (0 means no limit)

## Context

A stress test of 0.9.0 on a loopback sandbox (ASan and UBSan builds, 48,000 requests from 32 threads on free-threaded Python, malformed requests, certificates, WebSocket frames) found no memory error. What it found was missing limits and a few permissive defaults: 20 MB of `x=1&` used about 1.2 GB (56 times its size), a client ran until the kernel killed it on an endless body, a loopback server answered any `Host`, and `httpp curl http://a:b:c/` aborted. The report is not part of the repository; the numbers above are from one measurement each.

## Decision

1. **Pairs: at most 1024** in a query string or a urlencoded body (`detail::max_query_items`, the number the multipart parser already used for parts). The cost is the pair, not the byte, so the count is the limit that works. Over it, `make_request` refuses with a 400 and `request::form()` throws `std::invalid_argument`. Not configurable: no real form has more (Django 1000, Tomcat 10000).
2. **`max_form_body`, default 1 MiB,** for `application/x-www-form-urlencoded` only (nginx's `client_body_buffer_size`, Tomcat's `maxPostSize` is 2 MB). Over it, 413, read from the head or counted while chunks arrive. `0` leaves `max_body` alone. C++ `server::max_form_body`, Python `max_form_body`.
3. **`max_connections` default 10000** (Tomcat 8192). `0` is still no limit.
4. **HTTP/1.1 needs exactly one `Host`** (RFC 9112 3.2): none or two is a 400, empty is allowed. It is checked with the headers, before the body.
5. **Loopback servers check `Host`.** `httpp server` and `httpp browse` bound to `localhost`, `::1` or `127.x` install `trusted_host` with `localhost`, `127.0.0.1`, `[::1]` and the bound address, and no `www` redirect. The check is in the CLI, not in `server`, because a library user chooses the names. On `0.0.0.0` or a public address no list can be known, so nothing is refused; that is a limit of the protection, not a promise.
6. **`file_browser` refuses a cross-site POST** (`Origin` present and different from `Host`, or `null`) with a 403. Only POST: the zip route was the one that does work for a caller.
7. **Multipart names refuse control characters and direction marks** (the list is in the CHANGELOG).
8. **The TLS client requires `subjectAltName`** on the end-entity certificate, through a verify callback that sets the name-mismatch flag. The vendored mbedtls is not touched. A certificate for an IP address needs an IP SAN.
9. **Client limits: 256 MiB for a body `run()` collects, 64 KiB for the head.** A collected body costs about three times its size while it is read and decoded, so a larger default would not protect an 8 GB machine from a few responses. Streaming (`on_body`, `save_to`, `download`) has no body limit. 64 KiB is the size the server already allows for a request head.
10. **The CLI checks the URL with `parse_url` before the constructors,** which throw for a port that is not a number.

## Not done

- `X-Api-Key` and other custom headers still go to the target of a redirect to another host, as curl and requests do; only `Authorization`, `Cookie` and basic auth are dropped. Documented behavior, not changed.
- The TLS 1.2 CBC-SHA1 cipher suite is still accepted by the server; closing it breaks old clients and needs a compatibility policy first.
- `httpp::redirect()` does not check its target (`//evil.test`): the application's job.
- No coverage-guided fuzzing was run, and the Cython layer was not run under ASan.
