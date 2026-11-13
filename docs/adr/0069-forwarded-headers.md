# ADR-0069: Reading the client from a reverse proxy's headers

- **Status:** accepted; extends ADR-0037 (middleware)
- **Decided by:** the project owner ("go", after the proposal of `forwarded_headers` with trusted proxies); the details are Claude's
- **Source:** `include/httpp/middleware.hpp` (`forwarded_headers`), `include/httpp/server.hpp` (`server_request::client_ip`), `src/core/middleware.cpp`, `src/core/server.cpp` (`set_request_rewrite`), `src/core/async_server.cpp` (the peer address), `src/bindings/python/httpp/middleware.py` (`ProxyHeadersMiddleware`)

## Context

A request carried no client address at all: `server_request` had the method, path, headers and `secure`, and nothing that said who connected. Behind nginx, Caddy or a load balancer that is not enough, and the headers the proxy adds (`X-Forwarded-For`, `-Proto`, `-Host`) cannot simply be believed, because a client that reaches the port directly can send them too. The only existing support was `https_redirect(trust_forwarded_proto)`.

## Decision

- `server_request::client_ip` (Python `request.client_ip`): the address of the connection as text, `""` when unknown (IPv4-mapped IPv6 is shown as IPv4). The engine reads it once per connection.
- `httpp::forwarded_headers` (Python `ProxyHeadersMiddleware(trusted_proxies, trust_forwarded_host)`), builder style: `set_trusted_proxies(list)` and `set_trust_forwarded_host(bool)`, then `install(server&)`. An entry is an IPv4 or IPv6 address, a CIDR range, or `*`; the default is `127.0.0.1` and `::1`; anything else throws `std::invalid_argument` at the setter and leaves the object unchanged; an empty list trusts nobody.
- It acts only when the connection's own address is a trusted proxy, and then: `client_ip` becomes the first address of `X-Forwarded-For` from the right that is not itself a trusted proxy (the leftmost when all are; a value that is not an address is ignored), so a forged leftmost value never wins; `X-Forwarded-Proto` `https` / `http` (the last value) sets `secure`, and so `scheme()`; `X-Forwarded-Host` (the last value) replaces `Host` only with `set_trust_forwarded_host(true)`. A connection from anywhere else is left alone, whatever it sends.
- Mechanism: not a before hook (hooks get a `const` request). `server::set_request_rewrite` (private, `forwarded_headers` is its friend) runs a function on every request right after it is parsed and before any hook, on the thread that parses it, so `before`, `use()` middleware (`trusted_host`, `https_redirect`), routes and the after hooks all see the corrected request. One per server; a second install replaces the first.
- Not done: the RFC 7239 `Forwarded` header, `X-Forwarded-Port`, the PROXY protocol.

## Consequences

- With `forwarded_headers` installed, `https_redirect` no longer needs `trust_forwarded_proto` (it sees `secure`); the option stays.
- `trusted_host` sees the rewritten `Host` when `set_trust_forwarded_host(true)`.
- The earlier statement that a handler could read the client from `req.client` was wrong: no such field existed until this ADR.

## Verification

Sandbox, Linux, Release. C++ 542 passed (new: headers ignored without the middleware, trusted loopback gives client / scheme / host with the switch, the rightmost-untrusted rule with a forged leftmost value and a mixed v4/v6 CIDR list, an untrusted connection and an empty list and `*`, bad entries refused). Python 3.14t: 878 passed, 4 skipped (new: `ProxyHeadersMiddleware` end to end, bad entry `ValueError`).
Not verified: a connection from a non-loopback address (the tests can only connect from 127.0.0.1, so the CIDR matching of real peers is covered by the list logic, not by a real remote peer), an IPv6 peer on the wire, real nginx/Caddy in front, `set_io_threads` above 1, sanitizers, Windows (MSVC), macOS.
