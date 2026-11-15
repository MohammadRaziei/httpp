# ADR-0072: `httpp::url` is `urlparser::url`

- **Status:** accepted
- **Decided by:** the project owner (the wrapper class felt like bad practice; chose to strengthen liburlparser, which he also owns, and alias it); the implementation is Claude's
- **Source:** `include/httpp/url.hpp`, `src/core/url.cpp`, `tests/cpp/test_url.cpp`, `src/third_party/liburlparser` (2.3.0)

## Context

httpp had its own `httpp::url` class (scheme, host, port, path, query, userinfo, plus percent-encoding statics) that sat next to liburlparser, a lenient and fully featured parser already built into httpp. Two parsers
for one job, and `httpp::url` wrapped the second one. liburlparser had no default port, no decoded user info and no `std::string` constructor, which is why the wrapper existed.

## Decision

- liburlparser 2.3.0 gained what httpp needed: `url::default_port()`, `port_or_default()`, `username()`, `password()`, `std::string` and `const char*` constructors, the `URLPARSER_API` export macro, a shared build
  and the `URLPARSER_PSL` option.
- `httpp::url` is `using url = urlparser::url;`. What httpp accepts is httpp's policy, so it lives in free functions: `parse_url` (never throws), `url_valid`, `url_is_http`, `url_port` (explicit port, else 1080 for
  socks5, else the scheme default), `url_scheme`, `url_host` (IPv6 keeps its brackets), `url_username`, `url_password` (percent-decoded), `url_path` (dot segments resolved, `/` when empty), `url_query`.
- The codec statics became free functions with the Python names: `httpp::encode_component`, `decode_component`, `build_query` (Python `URL.encode_component` and so on, so the parity test needs no alias).
- liburlparser is built **shared** and without the embedded Public Suffix List. Shared, because the list is process-wide state: a static copy inside `libhttpp_core` and another in the application are two lists.
  The wheel SDK ships the library and `urlparser.h`.
- `httpp::fetch_public_suffix_list(source = "", timeout = 30)` (Python `URL.fetch_public_suffix_list`) downloads the list with httpp's own `request` (default address: `urlparser::psl::source_url()`)
  and loads it. It throws `std::runtime_error` (Python `RuntimeError`) on a failed request, a status other than 200, an empty body, an HTML page, or a list that does not parse; the loaded list is then unchanged.

## Rejected alternatives

Copying parser code into httpp (a third parser to maintain); exposing `urlparser::url` with httpp's rules inside it (httpp's policy does not belong in a general library); keeping the wrapper class (the thing the owner
objected to); a static liburlparser (two copies of the list, found by the test for `fetch_public_suffix_list`).

## Consequences

- **Breaking for C++ code (major is 0):** `httpp::url::parse`, `valid()`, `is_http()` and the static codec functions are gone; `host()` is `host_text()`, `scheme()` is `protocol()`, `path()` is `abspath()`; use the
  `url_*` functions where the old accessors encoded policy. A text with a bad port (`http://h:99999/`) now throws `std::invalid_argument` from the constructor; `parse_url` turns that into an invalid url.
  `str()` rebuilds the text instead of returning it as given.
- Without the list, `suffix` is the last label only, so `www.example.co.uk` has the suffix `uk`; call `fetch_public_suffix_list()` (or `psl::instance().load_from_string`) when `domain` splitting matters. httpp itself does not use it.
- The wheel is a little larger by one shared library.
- **Not verified:** Windows and macOS builds, the Cython module and the Python test suite in this change, the default address against the live internet (the test uses a local server).

## Related

ADR-0017 (C++ and Python APIs match), ADR-0062 (C++ names match Python's).
