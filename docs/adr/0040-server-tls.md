# ADR-0040: Server TLS (https and wss) on the engine, with the vendored Mbed TLS

- **Status:** accepted
- **Source:** `make_tls_server_context` and the server constructor in `src/core/tls_session.cpp`, the transport functions of `conn` in `src/core/async_server.cpp`, `server::set_tls`,
  `request::secure`, `add_https_redirect`, `Server.set_tls`, `httpp run --ssl-certfile/--ssl-keyfile/--ssl-keyfile-password`

## Context

The server only spoke plain HTTP, so https needed a proxy in front. The owner delegated the design (2026-11-06); zero system dependencies stays the first rule, and Mbed TLS is already vendored for the client.

## Decision

1. `server::set_tls(cert_file, key_file, key_password = "")` (Python `Server.set_tls`, `run(ssl_certfile=, ssl_keyfile=, ssl_keyfile_password=)`, `httpp run --ssl-*`). The PEM files are read and checked at once and the
   result is one immutable `tls_context` shared by every connection and I/O thread. Errors (`std::runtime_error`, Python `ValueError`) name the file: missing or unreadable file, bad PEM,
   wrong password, a key that does not belong to the certificate (`mbedtls_pk_check_pair`).
2. The TLS session is the existing memory-BIO `tls_session` with a server constructor. The engine gets a small transport layer in `conn`: `io_read` delivers plaintext into the read buffer and `io_write`
   writes plaintext, each a single `if (tls)` branch in front of the unchanged plain path, so the request parser, keep-alive, streaming, the tunnel (WebSocket, so wss), timeouts and the drain of
   ADR-0038 work unchanged on both. The handshake runs inside the first read of a connection. Ciphertext goes through one write queue per connection (handshake bytes, alerts and data share the wire in order).
   A connection closing sends `close_notify` best effort. A plain request to a TLS port fails the handshake and is dropped (no HTTP answer, no 503).
3. TLS 1.2 and 1.3 with Mbed TLS defaults, no client certificates, no session resumption or tickets (a handshake per connection; keep-alive is what saves the cost). One port serves TLS or plain HTTP, never both.
4. `request::secure` and `scheme()` ("https" or "http"; Python `request.secure`, `request.scheme`). `trusted_host`'s www redirect uses the request's scheme.
5. `add_https_redirect(trust_forwarded_proto = false)` / `HTTPSRedirectMiddleware`: a request that is not secure gets a 307 to `https://<Host header>/path?query` (port kept except 80); with `trust_forwarded_proto` an
   `X-Forwarded-Proto: https` header counts as secure, for a plain server behind a TLS-terminating proxy. It is off by default because a client that reaches the port directly could claim any scheme.
6. The listening log line says https. `--workers` and `--reload` pass the options on to the child processes.

## Rejected alternatives

- A TLS-terminating thread or a separate TLS engine: another threading model and a copy per byte; the memory BIO inside the connection costs one copy and keeps everything on the I/O thread.
- Both TLS and plain on one port (sniffing the first byte): possible, but it makes `request.secure` and redirect rules subtle and is rarely wanted; two servers on two ports do it.
- OpenSSL: a system dependency.
- Reloading certificates on a signal or by watching files: `ponytail:` not now; restart (or call `set_tls` again before listening). Upgrade path: swap the shared context atomically on SIGHUP.
- Session tickets and ALPN (h2): there is no HTTP/2, and resumption needs a ticket key shared across I/O threads and workers; left for a measured need.

## Consequences

- Plain connections pay one pointer check per read, write and request; no extra copy. TLS connections copy plaintext once into the read buffer and encrypt a whole answer piece at a time (`ponytail:` a multi-megabyte
  streamed piece is encrypted in one go on the loop thread; pieces are already bounded by the stream chunk size).
- Handshake time is bounded by the keep-alive/first-byte timeout (as the first byte of a plain request is); with that timeout set to 0 a stalled handshake holds a connection, as a silent plain connection does.
- Verified: 10 new C++ tests (https and secure flag, 1.5 MB bodies over many records on a kept-alive connection, a verifying and a non-verifying client, plain and garbage input on a TLS port, 12 concurrent clients on 2 I/O threads,
  WebSocket over TLS, shutdown draining a TLS request, `set_tls` errors, the redirects), 9 new Python tests (including real `httpp run --ssl-*`, with `--reload`, and with 2 workers, certificate made by the openssl command line);
  C++ suite 438 passed (Release; ThreadSanitizer: no warnings, 6 repeats of the TLS tests; AddressSanitizer and UBSan: no error, and no leak from the new code, the one leak report of the whole suite is the 18 KB in `async_http::impl::connection` at exit that was already known), Python suite 691 passed, 4 skipped, on 3.13.
  By hand, against a running `httpp run`: curl (TLS 1.3, TLS 1.2 only, `--http2` falling back to HTTP/1.1) and `openssl s_client`, with an EC P-256 and an RSA 2048 certificate, and with an encrypted
  PKCS#8 key (PBES2, AES-128 and AES-256, SHA-1 and SHA-256 PRFs). Found on the way: `mbedtls_pk_check_pair` fails for RSA keys in this build (it signs through PSA), so the match is checked by comparing
  the two public keys as DER; and a key encrypted with 3DES (what `openssl req` writes by default) is not supported, the error says so.
  Not run: browsers, other TLS libraries' clients than OpenSSL and Mbed TLS, performance against plain, client certificates, Windows and macOS, Python 3.14 and 3.14t.
