# ADR-0028: The orchestrator calls the Autobahn workflow

## Status
Accepted (2026-10-30). The owner agreed after the options were explained.

## Context
`autobahn.yml` ran on its own on every push to master, next to `orchestrator.yml`. Both touch the `__beta__` release: the orchestrator's
`init` job deletes all its assets, `autobahn.yml` uploads `autobahn-report.md` to it. The two started together, so the report could be
uploaded first and then deleted. Nothing ordered them.

## Decision
`autobahn.yml` is a reusable workflow (`workflow_call`, `workflow_dispatch` kept for manual runs, the `push` trigger removed).
`orchestrator.yml` calls it as a job with `needs: [init]` and `permissions: contents: write`, so the upload always comes after the clean-up.
`release-wheels` does not depend on it: a FAILED conformance case turns the autobahn job red but does not stop the wheels from being published.

## Rejected alternatives
- `workflow_run` trigger: a second run per push, detached from the orchestrator, and the ordering still depends on timing of the first run.
- Making `release-wheels` wait for autobahn: the report has never been produced yet, so it should not gate a release.
- Leaving it as it was: the race stays.

## Consequences
- The overall orchestrator run shows red when a case fails, even if the release was published.
- A pushed tag also runs it (the orchestrator triggers on `v*` tags): one extra job of a few minutes.
- **Not verified:** nothing was run, the sandbox has no docker and no GitHub Actions. The first push is the first real run.

## Related
ADR-0026, ADR-0027.
