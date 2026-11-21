# ADR-0067: URLs: every URL parameter is an `httpp::url` (an alias of `urlparser::url`), and a proxy is one URL

- **Status:** accepted (merged ADR; each part keeps its own status line)
- **Merged on:** 2026-11-21, at the request of the project owner: the decisions of one subject were spread over 2 ADRs. The text of every part is kept, as a section below, in the order it was decided.

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history. Where a part and this summary disagree, this summary wins.

- **Every public function that needs a URL takes `const url&`,** with implicit construction from `std::string` and `const char*` that never throws (a bad text gives `valid() == false`).
- **A proxy is one URL,** `http://[user[:pass]@]host[:port]`; user and password become `Proxy-Authorization: Basic ...`. Python takes `proxy="http://user:pass@127.0.0.1:3128"` (a tuple raises `TypeError`, a bad URL `ValueError`).
- **`httpp::url` is `using url = urlparser::url;`** (liburlparser): the wrapper class of the first part is gone. httpp's own policy is in free functions (`parse_url`, `url_valid`, `url_is_http`, `url_port`, `url_scheme`, `url_host`, `url_username`, `url_password`, `url_path`, `url_query`), and the codec statics are `encode_component`, `decode_component` and `build_query`.
- **liburlparser is built shared and without the embedded Public Suffix List** (the list is process-wide state); `httpp::fetch_public_suffix_list` downloads it with httpp's own `request`.

## Parts
### Every URL parameter is an `httpp::url`, and a proxy is one URL

- **Status:** accepted
- **Decided by:** the project owner ("make the API of both languages alike; `set_proxy("http://user:pass@127.0.0.1:3128")`; the argument should be `httpp::url` with an implicit constructor from string, for every place that needs a URL"); the details are Claude's
- **Source:** `include/httpp/url.hpp`, `include/httpp/client.hpp`, `include/httpp/curl.hpp`, `include/httpp/download.hpp`, `src/core/{url,client,curl,download,http_exchange}.cpp`, `src/bindings/python/cy/{client,transfer,declarations}.pxi`, `src/bindings/python/httpp/_http.py`

#### Context

The proxy was given as two values: C++ `set_proxy(host, port)` and `request::proxy(host, port)`, Python `proxy=(host, port)` in constructors but `Request.proxy(host, port)` as a method. Two shapes for one value, no way to give credentials (the engine could send `Proxy-Authorization`, but nothing set it), and no ADR explained it. Elsewhere URLs were plain `std::string`, so each function re-parsed and re-validated its own text.

#### Decision

- `httpp::url` has implicit constructors from `const std::string&` and `const char*` (and a default one). They never throw: a text that is not a usable http(s) URL gives `valid() == false`, and `str()` returns the text as given, for error messages. `url::parse` stays.
- `url` gains `username()` and `password()` (percent-decoded; empty when absent); Python's `URL` has the same two attributes.
- Every public function that needs a URL takes `const url&`: `client(base_url)`, `async_client(base_url, ...)`, `client::fetch`, `request(url)` (both `httpp::request` and `httpp::curl::request`), `download(url, ...)`, `download::sha256_url`, and the proxy setters. A string or a literal converts at the call, so callers and tests keep compiling; a url parsed earlier is passed as it is. Behaviour for an invalid URL is unchanged (the same `std::invalid_argument` or `invalid_url` response).
- The proxy is one URL: `client::set_proxy`, `async_client::set_proxy`, `request::proxy` take `http://[user[:pass]@]host[:port]`. The user and password become `Proxy-Authorization: Basic ...` (plain http through the proxy, and the CONNECT request of the tunnel for https). `""` removes the proxy. Anything else (`host:port`, `https://`, `socks5://`) throws `std::invalid_argument` at the setter and changes nothing. The two-argument forms are removed, no aliases (version 0.x).
- Python: `proxy="http://user:pass@127.0.0.1:3128"` in `Client`, `AsyncClient` and `httpp.get(...)`, and `Request.proxy(url)`. A tuple raises `TypeError`, a bad URL `ValueError`. The parity test checks the keyword, as before.

#### Consequences

- Breaking: C++ `set_proxy("h", 3128)` becomes `set_proxy("http://h:3128")`; Python `proxy=("h", 3128)` becomes `proxy="http://h:3128"`.
- Proxy authentication (Basic) now works; it had no way in before.
- SOCKS5 is added by ADR-0068. Still not supported: an https proxy, Digest/NTLM proxy authentication, and anything on the server side (`X-Forwarded-For`, `Forwarded`, `X-Forwarded-Host`). Those need their own ADR.
- **The environment (2026-11-18).** `httpp::proxy_from_environment(target)` and `httpp::resolve_proxy(target, proxy, no_proxy)` (`httpp/proxy.hpp`; Python `httpp.proxy_from_environment(url)` and `httpp.resolve_proxy(url, proxy=None, no_proxy=None)`, returning a proxy URL string or `None` for a direct connection) read the variables the way curl does: `http_proxy` for http:// (lowercase only, `HTTP_PROXY` is ignored because of httpoxy), `https_proxy` or `HTTPS_PROXY` for https://, `all_proxy` or `ALL_PROXY` as the fallback, an empty value counts as unset, and a value without a scheme is an http proxy (`127.0.0.1:10808`). `no_proxy` or `NO_PROXY` lists the hosts that go direct: `*`, names (the name and its subdomains, a leading `.` ignored, case-insensitive, matched at a label so `example.com` does not match `notexample.com`), IPv4 addresses and `a.b.c.d/n` ranges, separated by commas or spaces. `resolve_proxy` takes the same inputs as `-x` and `--noproxy` (an empty `proxy` is no proxy at all, an empty `no_proxy` excludes nothing, the list also applies to a given proxy). An unusable value throws `std::invalid_argument` (Python `ValueError`) naming it. ponytail: no IPv6 ranges and no `host:port` entries in the list.
- A request still does not read the environment on its own: a library should not send traffic to a proxy by surprise, and tests must not depend on the machine they run on. `proxy()` stays the one switch, and `httpp curl` (ADR-0077), which replaces curl in scripts, calls these functions. `httpp download` does not read the environment yet.
- Rejected: reading the environment inside `request` by default (surprising for a library, and it makes the same program behave differently on two machines); doing it only in the CLI (Python users would copy the `no_proxy` rules, which are the fiddly part); also reading `HTTP_PROXY` (httpoxy: a CGI server turns the `Proxy` request header into it). The Python result is put together from the parts of the proxy, not taken from `url.str()`, which normalizes a SOCKS5 address into a form a request cannot read back.

#### Verification

Sandbox, Linux, Release. C++ 531 passed (new: url conversion and `str()`, decoded user and password, the proxy URL reaches the proxy with the right `Proxy-Authorization`, bad proxies refused and the earlier one kept, `request` and `async_client` through the same URL, `set_proxy("")`). Python 3.14t: 875 passed, 4 skipped (new: proxy is one URL in every client, tuple `TypeError`, bad `ValueError`, `URL.username/password`).
Not verified: an IPv6 proxy host (`url::host()` keeps the brackets, as before), a real HTTPS tunnel through a real proxy, sanitizers, Windows (MSVC), macOS, Python 3.13 and 3.14 with a GIL.

### `httpp::url` is `urlparser::url`

- **Status:** accepted
- **Decided by:** the project owner (the wrapper class felt like bad practice; chose to strengthen liburlparser, which he also owns, and alias it); the implementation is Claude's
- **Source:** `include/httpp/url.hpp`, `src/core/url.cpp`, `tests/cpp/test_url.cpp`, `src/third_party/liburlparser` (2.3.0)

#### Context

httpp had its own `httpp::url` class (scheme, host, port, path, query, userinfo, plus percent-encoding statics) that sat next to liburlparser, a lenient and fully featured parser already built into httpp. Two parsers
for one job, and `httpp::url` wrapped the second one. liburlparser had no default port, no decoded user info and no `std::string` constructor, which is why the wrapper existed.

#### Decision

- liburlparser 2.3.0 gained what httpp needed: `url::default_port()`, `port_or_default()`, `username()`, `password()`, `std::string` and `const char*` constructors, the `URLPARSER_API` export macro, a shared build
  and the `URLPARSER_PSL` option.
- `httpp::url` is `using url = urlparser::url;`. What httpp accepts is httpp's policy, so it lives in free functions: `parse_url` (never throws), `url_valid`, `url_is_http`, `url_port` (explicit port, else 1080 for
  socks5, else the scheme default), `url_scheme`, `url_host` (IPv6 keeps its brackets), `url_username`, `url_password` (percent-decoded), `url_path` (dot segments resolved, `/` when empty), `url_query`.
- The codec statics became free functions with the Python names: `httpp::encode_component`, `decode_component`, `build_query` (Python `URL.encode_component` and so on, so the parity test needs no alias).
- liburlparser is built **shared** and without the embedded Public Suffix List. Shared, because the list is process-wide state: a static copy inside `libhttpp_core` and another in the application are two lists.
  The wheel SDK ships the library and `urlparser.h`.
- `httpp::fetch_public_suffix_list(source = "", timeout = 30)` (Python `URL.fetch_public_suffix_list`) downloads the list with httpp's own `request` (default address: `urlparser::psl::source_url()`)
  and loads it. It throws `std::runtime_error` (Python `RuntimeError`) on a failed request, a status other than 200, an empty body, an HTML page, or a list that does not parse; the loaded list is then unchanged.

#### Rejected alternatives

Copying parser code into httpp (a third parser to maintain); exposing `urlparser::url` with httpp's rules inside it (httpp's policy does not belong in a general library); keeping the wrapper class (the thing the owner
objected to); a static liburlparser (two copies of the list, found by the test for `fetch_public_suffix_list`).

#### Consequences

- **Breaking for C++ code (major is 0):** `httpp::url::parse`, `valid()`, `is_http()` and the static codec functions are gone; `host()` is `host_text()`, `scheme()` is `protocol()`, `path()` is `abspath()`; use the
  `url_*` functions where the old accessors encoded policy. A text with a bad port (`http://h:99999/`) now throws `std::invalid_argument` from the constructor; `parse_url` turns that into an invalid url.
  `str()` rebuilds the text instead of returning it as given.
- Without the list, `suffix` is the last label only, so `www.example.co.uk` has the suffix `uk`; call `fetch_public_suffix_list()` (or `psl::instance().load_from_string`) when `domain` splitting matters. httpp itself does not use it.
- The wheel is a little larger by one shared library.
- **Not verified:** Windows and macOS builds, the Cython module and the Python test suite in this change, the default address against the live internet (the test uses a local server).

#### Related

ADR-0016 (C++ and Python APIs match), ADR-0062 (C++ names match Python's).
