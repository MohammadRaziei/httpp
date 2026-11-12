# ADR-0064: The docs site is published to GitHub Pages

- **Status:** proposed (the build is checked locally with the real base URL; waiting for the first deploy)
- **Decided by:** the project owner (https://mohammadraziei.github.io/httpp showed GitHub's 404 page: nothing was published)
- **Source:** `docs/site/docusaurus.config.ts`, `.github/workflows/pages.yml`, ADR-0060

## Context

ADR-0060 built the site and said the workflow does not publish; `url` was `https://example.invalid`. So the address of the project site had nothing behind it.

## Decision

- `url` is `https://mohammadraziei.github.io`, `baseUrl` is `/httpp/` (a project site lives under the repository name).
- `.github/workflows/pages.yml` builds `docs/site` and deploys it with the official Pages actions on pushes to master that touch the site or `benchmarks/results/*.json`, and on demand. `docs.yml` stays the check and does not publish.
- One manual step in the repository: Settings -> Pages -> Source: GitHub Actions. Without it the deploy job fails and the address keeps its 404.

## Rejected alternatives

- **Deploying from `docs.yml`.** The orchestrator calls it with `workflow_call` and its permissions would have to grow for every caller; a separate workflow keeps the check read-only.
- **A `gh-pages` branch.** It puts build output in the history.

## Consequences

- Every link inside the site is relative to `/httpp/`; a custom domain later means changing `url` and `baseUrl` here.
- Not verified: the first real deploy, the Pages setting of the repository.

## Related

ADR-0060, ADR-0063.
