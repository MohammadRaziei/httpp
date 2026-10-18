# ADR-0011: Logging: AixLog behind `HTTPP_LOG`; request and bind-address logs in C++; defaults

- **Status:** accepted (the defaults were decided in this session; the owner left the method to me and accepted `run`)
- **Date:** 2026-10-18
- **Source:** `src/core/log.cpp`, `src/core/async_server.cpp`, `src/core/server.cpp`, `CHANGELOG.md`

## Context

`python app.py` printed nothing: neither the bound address nor the requests. The library's default log level is `warning`, and
the `starting on` message that `run` printed came before the bind and also showed when the bind failed.

## Decision

- Internal logging stays AixLog behind `HTTPP_LOG` (only `log.cpp` includes its header); the request log goes through the same path,
  so `set_log_level`, `set_log_file` and `set_log_callback` apply to it.
- One `info` line per response in `conn::send` (the point every response goes through, including the 404 and 413 the server makes itself):
  `GET /items/1 200`. The method and target are saved before the `std::move` and only when the `info` level is on.
- After a **successful bind**, `listening on http://host:port` (`info`), in both `listen` and `bind_to_any_port` (with the real port).
- `httpp.run` / `app.run()` / `httpp run` default to `info` (like uvicorn); the old `starting on` message was removed.
  `app.listen()` and the C++ API keep the default `warning`.

## Rejected alternatives

- **A Python `after_request` hook for logging:** Python and GIL cost on every request; used only as a temporary workaround in the example.
- **`info` as the default for the whole library:** an embedded server must not write to stderr without being asked, and it changes benchmarks.
  If `listen()` should also be chatty, this default has to change.

## Consequences

- Every `info` line costs something under load (a lock and a write to stderr); benchmarks must run with `log_level="warning"` or `listen()`.
- If the method is not known before parsing finishes (a 400 error), the log line starts with an extra space.
- Tests: 3 C++ tests in `test_log.cpp` (on Linux and macOS in CI), `test_log.py` (in every wheel) and the CLI test in
  `test_run.py`, which is skipped on Windows because `SIGTERM` there is a hard kill ([ADR-0015](0015-testing-and-ci.md)).

## Related

ADR-0003, ADR-0012
