# ADR-0077: `httpp curl` gets the everyday flags, and `httpp/curl.hpp` is retired

- **Status:** accepted
- **Decided by:** the project owner ("add the cheap flags first", "enrich `httpp::request`, not `httpp::curl::request`", "retire `httpp::curl::request` and all of `include/httpp/curl.hpp`", "our API is very good as it is")
- **Source:** `src/cli/httpp_cli.cpp`, `tests/python/test_cli.py`, `tests/python/test_cpp_python_parity.py`; removed: `include/httpp/curl.hpp`, `src/core/curl.cpp`, `tests/cpp/test_curl.cpp`

## Context

`httpp::curl::request` was a curl-flavoured builder with six methods (`method`, `header`, `data`, `content_type`, `timeout`, `follow_redirects`), a thin face on `httpp::request` (ADR-0053). `httpp::request` has all of them and more (auth, cookies, proxy, TLS options, JSON and form bodies, async). Nothing used the curl class: the `httpp curl` command already built an `httpp::request`, and the parity test only mapped it to `httpp.Request`. It was a second, weaker name for the same thing.

The `httpp curl` command took only `-X`, `-H`, `-d`, `-o`. It could not show the status or the headers of an answer, could not follow a redirect or set a timeout although `httpp::request` can, and it added a `\n` after every body, which corrupts binary output.

## Decision

- `httpp curl` gets `-i/--include` (status line and headers before the body), `-I/--head` (send `HEAD`, print the head only), `-v/--verbose` (the request line and the `-H` headers, then the response status and headers, on stderr), `-L/--location`, `-m/--max-time SECONDS` (a whole number, at least 1) and `-u/--user USER:PASSWORD` (basic authentication; only the first colon splits, a missing colon is an error).
- The head is printed as `HTTP/1.1 <status>` followed by the headers, with CRLF line ends. The engine speaks HTTP/1.1 and the response carries neither a version nor a reason phrase, so none is invented. `-v` prints only the headers given with `-H`, not the ones the engine adds itself (such as `Host`).
- `httpp curl` and `httpp download` (to stdout) no longer add a newline after the body, and stdout is switched to binary mode on Windows.
- `httpp/curl.hpp`, `httpp::curl::request`, `curl.cpp` and `test_curl.cpp` are removed. There is no replacement: use `httpp::request`. The libcurl-style C API in `httpp/curl_compat.h` is a different thing and stays.
- Where `httpp::request` is missing something an everyday request needs, it is added to `httpp::request` (and so to Python's `Request`), never to a second class.

## Consequences

- **Breaking for C++ code (major is 0):** `#include "httpp/curl.hpp"` and `httpp::curl::request` no longer exist; the same calls work on `httpp::request`.
- The parity test lost its `("curl.hpp", "request")` entry.
- Scripts that relied on the extra trailing newline of `httpp curl` or `httpp download` see the body byte for byte now.
- Not changed here and still different from curl: a non-2xx answer exits with 1 (curl needs `-f` for that), a transport error says only `status 0`, `-d` takes one value. These are the next step.

## Related

ADR-0041, ADR-0051, ADR-0053, ADR-0062, ADR-0067
