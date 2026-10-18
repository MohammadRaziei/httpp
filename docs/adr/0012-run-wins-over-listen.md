# ADR-0012: `httpp run` has priority; a `listen`/`run` inside the file is skipped, with a warning

- **Status:** accepted (proposed by the project owner; the warning was also his request)
- **Date:** 2026-10-18
- **Source:** `src/bindings/python/httpp/server.py` (`_defer_listen`, `_skipped`), `_run.py`, `tests/python/test_run.py`

## Context

A file with `app.listen(...)` and no `if __name__ == "__main__":` blocks at import time under `httpp run app:app`:
`--port`/`--host` were silently ignored, `--workers 2` died with a bind error (exit code 1) and `run` never reached its own line.

## Decision

When `run` imports a **string** target `"module:attr"`, an internal flag is on and `Server.listen()` and `Server.run()` only write a warning
to stderr and return:
`httpp: ignoring app.listen('127.0.0.1', 8001): `httpp run` decides host, port and workers (call it under `if __name__ == "__main__":` ...)`.
After the import the flag is turned off and `run` listens with its own `--host/--port/--workers`. `python file.py` is unchanged.

## Rejected alternatives

- **Doing nothing** (the old behavior: silent and fragile).
- **Raising an error:** old files (8 of the repo's 15 examples call `listen` without a guard) would break under `httpp run`.
- **Skipping silently:** the user would not know why the file's setting was not applied.

## Consequences

- Only a string target is affected; `httpp.run(app_object)` imports no file.
- Only `listen` and `run` are skipped; `listen_after_bind` and `bind_to_any_port` (used by tests) are untouched.
- The flag is a global variable (`ponytail:`); importing several apps at once in different threads is not planned on this path.
- On Windows the import-time test runs with `workers=1` but does not check the exit code (SIGTERM is a hard kill there);
  the `workers=2` case runs only on Linux ([ADR-0015](0015-testing-and-ci.md)).

## Related

ADR-0005, ADR-0011
