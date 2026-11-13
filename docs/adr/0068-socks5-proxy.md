# ADR-0068: SOCKS5 proxies

- **Status:** accepted; extends ADR-0067
- **Decided by:** the project owner ("start SOCKS"); the details are Claude's
- **Source:** `src/core/async_http.cpp` (`socks_handshake`, `socks_request`), `src/core/http_exchange.cpp` (`parse_proxy_url`), `src/core/internal/{async_http,http_exchange}.hpp`, `src/core/url.cpp`, `tests/cpp/test_async_pool_proxy.cpp`

## Decision

- A proxy URL may be `socks5://[user[:pass]@]host[:port]` or `socks5h://...` (default port 1080), in `set_proxy`, `request::proxy` and Python `proxy=`. Same URL shape as the HTTP proxy of ADR-0067; nothing else in the API changes.
- `socks5`: the target name is resolved here and its address goes to the proxy. `socks5h`: the name goes to the proxy, which resolves it (no DNS leak). An IP literal is always sent as an address. This is the convention of curl and `requests`.
- RFC 1928 with the "no authentication" and RFC 1929 "user/password" methods, CONNECT only. The handshake runs on the loop thread in the same connect stage as the HTTP CONNECT tunnel; afterwards the socket is a plain tunnel, so https works exactly as through an HTTP tunnel (TLS on top) and plain http is sent in origin form (never absolute form, which is for HTTP proxies).
- A user or password over 255 bytes (the protocol limit) throws at the setter. socks4/4a, an https proxy, UDP ASSOCIATE and GSSAPI are not offered and throw.
- `httpp::url` now parses the schemes `socks5` and `socks5h` (valid(), default port 1080) so a proxy URL goes through the same parser as every other URL. `url::is_http()` says whether a URL can be fetched; every fetching path (`client`, `async_client`, `fetch`, `request`, `download`, redirects) checks `is_http()`, so `client("socks5://...")` is still refused. Python `URL` has `is_http` too.
- The connection pool key includes the proxy kind and the SOCKS credentials (they shape the tunnel), so a tunnel is never reused across users.

## Verification

Sandbox, Linux, Release, against a SOCKS5 server written for the tests (it records what was asked): no auth, user/password accepted and refused, a server that wants a login and a client without one, `socks5h` sending the name (ATYP 3), `socks5` sending an address, refused connection (reply 5), a non-SOCKS server, https inside the tunnel with the pool reusing it (one tunnel, two requests), percent-encoded credentials through `client::set_proxy`, bad proxy URLs refused with the earlier proxy kept. C++ 537 passed; Python tests in `tests/python/test_builders.py`.
Not verified: a real SOCKS5 proxy (`ssh -D`, Tor, a commercial one; the sandbox has no such network), IPv6 proxy hosts, a name that does not resolve (the sandbox resolver hangs until the timeout, so it is not asserted), the 255-byte host name limit end to end, sanitizers, Windows (MSVC), macOS.
