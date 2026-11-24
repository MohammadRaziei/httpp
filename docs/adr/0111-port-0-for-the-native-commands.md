# ADR-0111: `httpp server` and `httpp browse` accept `--port 0`

- **Status:** built and run on Linux (the `test_cli.py` and `test_loopback.py` tests pass serial and with `pytest -n 4 --dist loadfile`); Windows and macOS not run
- **Date:** 2026-11-23
- **Decided by:** the project owner, after asking whether the tests are safe under pytest-xdist.

## Decision

- `--port` of `server` and `browse` takes 0 to 65535 (it was 1 to 65535). 0 asks the system for a free port; the line printed once the socket is bound (`Serving '...' at http://127.0.0.1:PORT/`) and the `listening on` log line say which.
- The tests of `test_cli.py` that start a server no longer pick a port first. They start it with `--port 0` and read the port from that line (`announced_port`).

## Why

`free_port()` binds a socket, reads its port and closes it; the command then binds the same number. Between the two, another process (another xdist worker, any program on the machine) can take it. With `--port 0` the system hands the port over at the moment of the bind, so there is no window.

Ports for a command that must **not** be listening (a "dead" address for `curl`) still use `free_port()`: a taken port there at worst makes a test see an answer instead of a refusal, and the chance is the same as before.

## Not changed

- `httpp py run` and the Python tests that call `free_port()` (`test_run.py`, `test_shutdown.py`, `test_reload.py`, `test_tls.py`, `test_file_browser.py`, `test_websocket_router.py`) keep their way: `py run` takes `--port` from Python, and `0` there is a separate decision.

## Loopback tests

`tests/python/test_loopback.py` runs our server and our client on 127.0.0.1, on `::1` and through `localhost` (which may resolve to either, first). The IPv6 tests are skipped on a computer with no `::1`.

## A client bug the loopback tests found

CI (ubuntu) failed `test_ipv6_loopback_answers`: `http://[::1]:PORT/` ended in "cannot resolve host: Host not found". `url_host()` keeps the brackets of an IPv6 literal, and `connect()` in `async_http.cpp` handed that text to the resolver, which only knows `::1`. (The SOCKS code and `proxy.cpp` already took the brackets off by hand; `connect()` did not.) `connect()` now resolves, and gives the TLS session as its name, the host without brackets, for the target and for the proxy alike. `test_a_bracketed_ipv6_literal_is_connected_to_not_resolved` needs no IPv6 on the computer: it asks `[::1]:1` and requires a failure to connect, not to resolve.

### Where the brackets are taken off

`url_host()` stays the host as written, brackets included: a `Host:` header, a `CONNECT` line, an absolute-form proxy target and the connection pool key all need the URL's own text. Three places need the address and used to take the brackets off by hand, each its own copy: `connect()` (and the TLS name), `socks_request()` and `no_proxy_matches()`; the first had forgotten. liburlparser now owns that: `urlparser::bare_host(text)`, which works on text because a job's `host` and `proxy_host` are strings; for the host of a url it is `bare_host(u.host_text())`. The three places call it.

`httpp::url_port()` (ADR-0067) is gone: the port of a URL is `u.effective_port()` of liburlparser (called `port_or_default()` up to 2.3.1), and liburlparser's `default_port()` now also knows socks5 and socks5h (1080), the one rule httpp had added on top. Callers, the Cython layer and the tests use `effective_port()` directly. One visible change comes with it: liburlparser's `normalized()` drops a port equal to the scheme's default, so `socks5://h:1080/` normalizes to `socks5://h/`.
