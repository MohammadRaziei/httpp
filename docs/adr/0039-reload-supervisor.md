# ADR-0039: `--reload` as a polling supervisor process

- **Status:** accepted
- **Source:** `src/bindings/python/httpp/_reload.py`, `run(reload=...)`, `httpp run --reload`

## Context

During development the owner wants the server to pick up code changes without restarting by hand, like `uvicorn --reload`. The owner delegated the design (2026-11-06).

## Decision

1. `httpp run main:app --reload` (`httpp.run("main:app", reload=True)`) starts a supervisor that runs the app as a child `httpp run` process (own session on POSIX, so Ctrl+C reaches only
   the supervisor) and polls the watched files every 0.25 s.
2. Watched: files whose name matches `--reload-pattern` (default `*.py`, repeatable) under `--reload-dir` (default the current directory, repeatable). Skipped: hidden directories,
   `__pycache__`, `node_modules` and any directory containing `pyvenv.cfg`. A change is a new, removed or different (mtime in ns, or size) file.
3. On a change (after a 0.2 s settle, because editors write several files) the child gets SIGTERM, which is the graceful stop of ADR-0038 with a 1 second grace period (`--shutdown-timeout` overrides it),
   is killed if it does not leave within grace + 2 s, and a fresh child starts: the new code is imported in a new process, no module reloading tricks.
4. A child that dies (a syntax error at import, a crash) is reported once; the supervisor keeps watching and starts it again on the next change. SIGINT or SIGTERM on the supervisor stops the child and exits 0.
5. `--reload` needs an import string (the child imports the app) and one worker; with an app object it is a `TypeError`, with `--workers N` a `ValueError`.
6. Python and CLI only. There is no C++ API, as the feature is about restarting a process around Python code; the C++ CLI (checklist 6.1) will start the Python `run` for it like for `httpp run`.

## Rejected alternatives

- Reload modules inside the process (`importlib.reload`): leaves stale objects, open sockets and handler tables behind; a new process is simple and always right.
- A native watcher (inotify, FSEvents, ReadDirectoryChangesW): three code paths and a dependency or platform code for a development convenience. `ponytail:` polling instead. Upgrade path: a native watcher for very large trees.
- Restart the child on any file: would restart on logs, data and caches; the pattern list keeps it to code.

## Consequences

- A scan of a few hundred files is about a millisecond every 0.25 s, development only.
- A change made while the old child is still draining is picked up on the next scan after the restart finishes (the snapshot is taken before the stop).
- Files that are written continuously and match the patterns cause a restart loop; narrow the patterns or directories.
- Verified: 5 new Python tests (watcher snapshot and change detection, argument errors, a real process restarted on an edit, surviving a syntax error and serving again after the fix, SIGTERM exit 0 with the child gone, `--reload --workers 2` refused),
  then the whole Python suite on 3.13. Not run: Python 3.14 and 3.14t, Windows (child stop uses `terminate()`), macOS, a large tree.
