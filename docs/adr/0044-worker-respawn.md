# ADR-0044: The `--workers` supervisor restarts dead workers and workers die with it

- **Status:** accepted
- **Source:** `_WorkerGroup`, `_parent_death_hook` and `_supervise` in `src/bindings/python/httpp/_run.py`, `tests/python/test_run.py`
- **Supersedes:** the "a dead worker stops the group" rule of ADR-0005

## Context

ADR-0005 stopped the whole group when one worker died and left restarting to systemd or Docker. That is safe but costly: one crash (an out-of-memory kill of a single
worker, a bug that raises in a C extension) takes down every worker and every connection of a healthy service. A second gap was found with it: if the supervisor is killed
with SIGKILL, nothing tells its workers, so they keep the port for ever and a restarted supervisor cannot take it over cleanly.

## Decision

1. The supervisor keeps one slot per worker and starts a new process when the one in a slot dies. The other workers are not touched.
2. Back-off: the first death of a worker that lived less than 30 seconds is restarted at once; the next quick deaths in a row wait 0.5, 1, 2 and 4 seconds. A worker that lived
   at least 30 seconds before it died starts the count over (the death was probably an accident, not a broken app).
3. Crash loop: one more quick death after the 4 second wait (the sixth in a row for a slot) stops the whole group with exit code 1, as before. A broken app, a missing
   module or a port that cannot be bound would otherwise be restarted for ever and hidden behind a "healthy" supervisor.
4. Every restart and the give-up are logged to stderr with the worker's pid and how it ended (exit code or the signal name).
5. A worker asks the kernel (`prctl(PR_SET_PDEATHSIG, SIGTERM)` in a `preexec_fn`) to send it SIGTERM when its supervisor dies, SIGKILL included, so a worker never outlives its
   supervisor. SIGTERM is the graceful stop of ADR-0038. The hook checks the parent pid afterwards to close the race where the supervisor died between fork and the call.
   Where libc has no `prctl` the hook is skipped and the old behaviour remains.
6. No new option. The numbers are module constants (`_RESPAWN_DELAYS`, `_STABLE`).

## Rejected alternatives

- A `--no-respawn` or `--max-restarts` option: more surface in both the Python and the native CLI for a policy most users never change; the constants are easy to turn into options
  later if someone asks.
- Respawning for ever with a capped back-off (what gunicorn does): it hides a worker that can never start. A supervisor that gives up loudly is easier to run under systemd.
- Counting deaths in a time window instead of a streak: same result for the cases that matter, more state.
- A watchdog thread in each worker that polls `getppid()`: it needs the GIL and a poll interval, and it cannot run while the worker is blocked in C.

## Consequences

- `--workers` keeps serving through a single worker crash, with a short gap for the connections that were on the dead worker.
- Sticky state in a worker (module-level caches) is lost on a restart, as it is on any process restart.
- `ponytail:` no jitter and no per-slot limit on the total number of restarts over a long time (a worker that crashes every 40 seconds is restarted for ever). Upgrade path:
  a restart budget per hour, with an option.
- Verified (Linux, Python 3.13): policy tests with fake processes (first restart at once, growing waits, give-up after the last delay, streak reset after a long life),
  a real run with three workers where one is killed with SIGKILL (replaced, same port keeps answering, the supervisor then stops gracefully with exit 0 and every current worker
  ran its shutdown hook), a real run where the supervisor is killed with SIGKILL (every worker ends), and the old crash-loop test (all workers cannot bind: exit 1).
- Not verified: Python 3.14 and 3.14t, other libc than glibc (the musllinux wheel runs the same code), a long run with real crashes.
