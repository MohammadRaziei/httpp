# ADR-0060: The documentation site: one page per concept, a Python/C++ code switch, published to GitHub Pages

- **Status:** proposed (merged ADR; waiting for the first deploy and the owner's review)
- **Merged on:** 2026-11-21, at the request of the project owner: the decisions of one subject were spread over 2 ADRs. The text of every part is kept, as a section below, in the order it was decided.

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history. Where a part and this summary disagree, this summary wins.

- **A Docusaurus site in `docs/site/`,** one page per concept; the code on a page follows one language choice made in the top bar and remembered in the browser.
- **The code lives in real files** (`snippets/<lang>/<name>.<ext>`), never pasted into a page; `scripts/prepare.py check` fails when a snippet exists in one language only.
- **Published to GitHub Pages:** `url` is `https://mohammadraziei.github.io`, `baseUrl` is `/httpp/`; `.github/workflows/pages.yml` builds `docs/site` and deploys it on pushes to master that touch the site or `benchmarks/results/*.json`, and on demand. `docs.yml` stays the check and does not publish.

## Parts
### A documentation site with one page per concept and a Python/C++ code switch

- **Status:** proposed (seven pages; waiting for the first CI run and the owner's review)
- **Decided by:** the project owner ("document the project well; explain C++ and Python together, with a switch for the code"; the choice of tool was left to the assistant)
- **Source:** `docs/site/`, `README.md` (604 lines, today's only user documentation), `docs/adr/0016-dual-api.md`

#### Context

The user documentation is one long `README.md`, with Python first. The two APIs are close (ADR-0016) but not identical, and there was no place to explain them side by side. The repository has no Doxygen or Sphinx reference.

#### Decision

- A Docusaurus site in `docs/site/`. One page per concept; the code on the page follows one language choice made in the top bar and remembered in the browser.
- The code lives in real files (`snippets/<lang>/<name>.<ext>`), never pasted into a page. `scripts/prepare.py check` fails when a snippet exists in one language only.
- The language list is `docs/site/languages.json` only.
- Text that applies to one language goes in `<Only lang="...">`; the table "Where the two differ" on each page records the real API differences.

#### Rejected alternatives

- **MkDocs Material.** No Node, and synced content tabs exist, but there is no site-wide switch that also changes prose without custom JavaScript.
- **Keep growing the README.** It already has 604 lines and cannot show both languages at once.

#### Consequences

- Node joins the toolchain for the docs only. `.github/workflows/docs.yml` builds the site: the orchestrator calls it on every push to master and on tags, after the `cmake` job has passed (informational, like autobahn: `release-wheels` does not wait for it), and pull requests run it when `docs/site/` changes. Nobody needs Node to build the library. The workflow does not publish.
- Checked locally: `npm ci`, the type check and the site build pass; in Chromium the switch, the sync between pages, the stored choice, `?lang=`, dark mode and a phone width work; all 9 snippets in both languages build and behave as their pages say against a local build of this tree. Not checked: a CI run, the web fonts (no network here), Windows and macOS.
- Every C++ claim on a page was read from `include/httpp/*.hpp` and `tests/`; every Python claim from the README and `examples/python/`. The C++ columns of the "Where the two differ" tables are the part to review first.
- `docs/site/scripts/test_snippets.py` builds the C++ snippets (with the CMake recipe from the install page) and runs all of them against the installed package, in the docs workflow. It lives outside ctest on purpose: it needs the installed wheel, and it must not make the library's own build depend on the docs.

#### Related

ADR-0014 and 0016

### The docs site is published to GitHub Pages

- **Status:** proposed (the build is checked locally with the real base URL; waiting for the first deploy)
- **Decided by:** the project owner (https://mohammadraziei.github.io/httpp showed GitHub's 404 page: nothing was published)
- **Source:** `docs/site/docusaurus.config.ts`, `.github/workflows/pages.yml`, the part "A documentation site with one page per concept and a Python/C++ code switch"

#### Context

the part "A documentation site with one page per concept and a Python/C++ code switch" built the site and said the workflow does not publish; `url` was `https://example.invalid`. So the address of the project site had nothing behind it.

#### Decision

- `url` is `https://mohammadraziei.github.io`, `baseUrl` is `/httpp/` (a project site lives under the repository name).
- `.github/workflows/pages.yml` builds `docs/site` and deploys it with the official Pages actions on pushes to master that touch the site or `benchmarks/results/*.json`, and on demand. `docs.yml` stays the check and does not publish.
- One manual step in the repository: Settings -> Pages -> Source: GitHub Actions. Without it the deploy job fails and the address keeps its 404.

#### Rejected alternatives

- **Deploying from `docs.yml`.** The orchestrator calls it with `workflow_call` and its permissions would have to grow for every caller; a separate workflow keeps the check read-only.
- **A `gh-pages` branch.** It puts build output in the history.

#### Consequences

- Every link inside the site is relative to `/httpp/`; a custom domain later means changing `url` and `baseUrl` here.
- Not verified: the first real deploy, the Pages setting of the repository.

#### Related

ADR-0063 and the part "A documentation site with one page per concept and a Python/C++ code switch".
