# ADR-0005: Multi-process with fresh processes and `SO_REUSEPORT` (Linux only)

- **Status:** accepted
- **Decided by:** the project owner
- **Source:** `Architecture.md` section 6, `handoff.md`, `src/bindings/python/httpp/_run.py`

## Context

Because of the GIL, one Python process can use only one core for user code. Several processes must listen on one port.

## Decision

`httpp run app:app --workers N` (or `--workers auto`) starts N **fresh processes**, each importing the app itself and listening on the same
port with `SO_REUSEPORT`; a supervisor keeps them. `auto` means one worker per CPU the process may use (affinity mask,
capped by the cgroup v1/v2 quota, rounded down, at least 1).

## Rejected alternative

`fork`. Creating a `Server()` creates an Asio epoll fd that becomes shared between processes after a fork, which is not safe.

## Consequences

- Linux only. On other operating systems, or when `app` is an object (not a `"module:attr"` string), one process stays.
- Because each worker imports the app, `--workers` works only with a string target.
- A dead worker is not recreated: when one dies the whole group stops (`--reload` and respawn are not there yet).
- The multi-worker tests run only on Linux and are skipped on macOS and Windows ([ADR-0015](0015-testing-and-ci.md)).
- Memory shared between workers (`app.shared`) is not designed yet; its scope is an open question.

## Related

ADR-0001, ADR-0012
