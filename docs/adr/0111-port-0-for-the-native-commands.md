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
