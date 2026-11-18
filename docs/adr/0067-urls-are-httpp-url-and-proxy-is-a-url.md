# ADR-0067: Every URL parameter is an `httpp::url`, and a proxy is one URL

- **Status:** accepted
- **Decided by:** the project owner ("make the API of both languages alike; `set_proxy("http://user:pass@127.0.0.1:3128")`; the argument should be `httpp::url` with an implicit constructor from string, for every place that needs a URL"); the details are Claude's
- **Source:** `include/httpp/url.hpp`, `include/httpp/client.hpp`, `include/httpp/curl.hpp`, `include/httpp/download.hpp`, `src/core/{url,client,curl,download,http_exchange}.cpp`, `src/bindings/python/cy/{client,transfer,declarations}.pxi`, `src/bindings/python/httpp/_http.py`

## Context

The proxy was given as two values: C++ `set_proxy(host, port)` and `request::proxy(host, port)`, Python `proxy=(host, port)` in constructors but `Request.proxy(host, port)` as a method. Two shapes for one value, no way to give credentials (the engine could send `Proxy-Authorization`, but nothing set it), and no ADR explained it. Elsewhere URLs were plain `std::string`, so each function re-parsed and re-validated its own text.

## Decision

- `httpp::url` has implicit constructors from `const std::string&` and `const char*` (and a default one). They never throw: a text that is not a usable http(s) URL gives `valid() == false`, and `str()` returns the text as given, for error messages. `url::parse` stays.
- `url` gains `username()` and `password()` (percent-decoded; empty when absent); Python's `URL` has the same two attributes.
- Every public function that needs a URL takes `const url&`: `client(base_url)`, `async_client(base_url, ...)`, `client::fetch`, `request(url)` (both `httpp::request` and `httpp::curl::request`), `download(url, ...)`, `download::sha256_url`, and the proxy setters. A string or a literal converts at the call, so callers and tests keep compiling; a url parsed earlier is passed as it is. Behaviour for an invalid URL is unchanged (the same `std::invalid_argument` or `invalid_url` response).
- The proxy is one URL: `client::set_proxy`, `async_client::set_proxy`, `request::proxy` take `http://[user[:pass]@]host[:port]`. The user and password become `Proxy-Authorization: Basic ...` (plain http through the proxy, and the CONNECT request of the tunnel for https). `""` removes the proxy. Anything else (`host:port`, `https://`, `socks5://`) throws `std::invalid_argument` at the setter and changes nothing. The two-argument forms are removed, no aliases (version 0.x).
- Python: `proxy="http://user:pass@127.0.0.1:3128"` in `Client`, `AsyncClient` and `httpp.get(...)`, and `Request.proxy(url)`. A tuple raises `TypeError`, a bad URL `ValueError`. The parity test checks the keyword, as before.

## Consequences

- Breaking: C++ `set_proxy("h", 3128)` becomes `set_proxy("http://h:3128")`; Python `proxy=("h", 3128)` becomes `proxy="http://h:3128"`.
- Proxy authentication (Basic) now works; it had no way in before.
- SOCKS5 is added by ADR-0068. Still not supported: an https proxy, Digest/NTLM proxy authentication, and anything on the server side (`X-Forwarded-For`, `Forwarded`, `X-Forwarded-Host`). Those need their own ADR.
- **The environment (2026-11-18).** `httpp::proxy_from_environment(target)` and `httpp::resolve_proxy(target, proxy, no_proxy)` (`httpp/proxy.hpp`; Python `httpp.proxy_from_environment(url)` and `httpp.resolve_proxy(url, proxy=None, no_proxy=None)`, returning a proxy URL string or `None` for a direct connection) read the variables the way curl does: `http_proxy` for http:// (lowercase only, `HTTP_PROXY` is ignored because of httpoxy), `https_proxy` or `HTTPS_PROXY` for https://, `all_proxy` or `ALL_PROXY` as the fallback, an empty value counts as unset, and a value without a scheme is an http proxy (`127.0.0.1:10808`). `no_proxy` or `NO_PROXY` lists the hosts that go direct: `*`, names (the name and its subdomains, a leading `.` ignored, case-insensitive, matched at a label so `example.com` does not match `notexample.com`), IPv4 addresses and `a.b.c.d/n` ranges, separated by commas or spaces. `resolve_proxy` takes the same inputs as `-x` and `--noproxy` (an empty `proxy` is no proxy at all, an empty `no_proxy` excludes nothing, the list also applies to a given proxy). An unusable value throws `std::invalid_argument` (Python `ValueError`) naming it. ponytail: no IPv6 ranges and no `host:port` entries in the list.
- A request still does not read the environment on its own: a library should not send traffic to a proxy by surprise, and tests must not depend on the machine they run on. `proxy()` stays the one switch, and `httpp curl` (ADR-0084), which replaces curl in scripts, calls these functions. `httpp download` does not read the environment yet.
- Rejected: reading the environment inside `request` by default (surprising for a library, and it makes the same program behave differently on two machines); doing it only in the CLI (Python users would copy the `no_proxy` rules, which are the fiddly part); also reading `HTTP_PROXY` (httpoxy: a CGI server turns the `Proxy` request header into it). The Python result is put together from the parts of the proxy, not taken from `url.str()`, which normalizes a SOCKS5 address into a form a request cannot read back.

## Verification

Sandbox, Linux, Release. C++ 531 passed (new: url conversion and `str()`, decoded user and password, the proxy URL reaches the proxy with the right `Proxy-Authorization`, bad proxies refused and the earlier one kept, `request` and `async_client` through the same URL, `set_proxy("")`). Python 3.14t: 875 passed, 4 skipped (new: proxy is one URL in every client, tuple `TypeError`, bad `ValueError`, `URL.username/password`).
Not verified: an IPv6 proxy host (`url::host()` keeps the brackets, as before), a real HTTPS tunnel through a real proxy, sanitizers, Windows (MSVC), macOS, Python 3.13 and 3.14 with a GIL.
