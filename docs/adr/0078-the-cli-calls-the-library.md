# ADR-0078: the `httpp` commands call the library; `mount` returns the server

- **Status:** accepted; the first decision (`mount` returns the `server&`) is superseded by ADR-0081 (`mount` takes and returns a `server` handle by value)
- **Decided by:** the project owner (the commands should only call functions, not hold logic)
- **Source:** `include/httpp/server.hpp` (`mount`, `bind`, `url`, `listen_until_signal`), `include/httpp/banner.hpp`, `include/httpp/logging.hpp` (`set_log_level(name)`), `src/core/tls_session.cpp`, `src/cli/httpp_cli.cpp` (`run_server`), `tests/cpp/test_server_lifecycle.cpp`

## Context

ADR-0041 made `httpp` a command over the library. In practice `server` and `browse` had grown logic of their own that a C++ program would want too: the signal handlers and the drain loop, the table that turns `--log-level info` into a level, the `http://host:port/` string, the hint for an encrypted key without its password, and the banner. A program embedding the library had to copy them.

## Decision

- **`mount` returns the server it was given.** `directory::mount`, `file_browser::mount` and `swagger_ui::mount` return `server&`, a reference to the argument (no copy; `server` is move-only), like `set_tls` already does. The chain goes on: `directory(p).mount(srv, "/").set_tls(cert, key)`. Existing calls that ignore the result compile unchanged.
- **`server::bind(host, port)`** binds without listening, so the address is known before the call that blocks. `listen(host, port)` is now `bind` then `listen_after_bind`. The "listening on ..." log line moved to the bind, so a port that is taken never says it.
- **`server::url()`** is `scheme://host:port/`: https after `set_tls`, the port that was really bound (so `bind(host, 0)` works), an IPv6 host in brackets (the CLI wrote `http://::1:8000/`, which is not a URL). It is `""` before a bind. The host is the one that was asked for, so `0.0.0.0` stays `0.0.0.0`.
- **`server::listen_until_signal(grace)`** is what the CLI's `serve()` was: after `bind()`, the first SIGINT/SIGTERM is `shutdown(grace)`, a second is `stop()`. It replaces the process's two handlers while it runs and puts the old ones back before it returns, so it belongs in one place per process.
- **`set_log_level(std::string_view)`** takes `trace`, `debug`, `info`, `warning`, `error` or `off` in any case, and returns false for another name. `level_from_string` could not serve: it belongs to the other level enum and does not know `off`.
- **`httpp::banner()` and `httpp::print_banner(FILE*)`** (`banner.hpp`) hold the mark, the version and the terminal test. Printing it is opt-in; the library never prints it by itself, because a program that embeds the library, or Python's `Server`, must not get ASCII art on stderr.
- **The hint for an encrypted key without its password** is in the core, where the key text is already in memory, not in the CLI: `set_tls` says the key is encrypted and no password was given.
- The CLI keeps what is about its own command line: the options, their defaults and checks, the exit codes, the environment variable for the key password, and the decision to show the banner (`--no-banner`, `--log-level off`). `run_server` is the one function the two commands share, and it is six calls.

## Rejected alternatives

- **`directory::make_server()`.** A second way to the same result under a new name; `mount` already takes the server and the rest of the builders return `*this`.
- **`mount(base)` with no server argument, creating one.** It could not mount several things on one server, and it leaves open who owns the server.
- **Printing the banner from `listen`.** One surprise for every embedder.
- **Binding the new names in Python.** `Server.run` already binds, serves and stops on a signal, and Python knows the host and port it passed. The three names are in `NOT_EXPOSED` with that reason (ADR-0017), and `banner.hpp` maps to `None`.

## Consequences

- A C++ program gets the same lifecycle as the commands in a few calls.
- The log line now says `(bind)` instead of `(listen)` as its source.
- `mount`'s return type changed from `void`: source compatible, but not binary compatible, and the library is still before 1.0.
- `url()` and the IPv6 brackets are tested for a machine with IPv6 loopback only (the test returns early without one; this machine had none, so the bracket case was never run).
- `listen_until_signal` installs process-wide handlers. Two servers listening this way at once would share one signal counter.

## Related

ADR-0041 (the `httpp` command), ADR-0040 (TLS), ADR-0076 (HTTPS for the two commands), ADR-0017 (`NOT_EXPOSED`).
