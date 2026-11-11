# ADR-0060: A documentation site with one page per concept and a Python/C++ code switch

- **Status:** proposed (seven pages; waiting for the first CI run and the owner's review)
- **Decided by:** the project owner ("document the project well; explain C++ and Python together, with a switch for the code"; the choice of tool was left to the assistant)
- **Source:** `docs/site/`, `README.md` (604 lines, today's only user documentation), `docs/adr/0016-hybrid-api-parity.md`

## Context

The user documentation is one long `README.md`, with Python first. The two APIs are close (ADR-0016) but not identical, and there was no place to explain them side by side. The repository has no Doxygen or Sphinx reference.

## Decision

- A Docusaurus site in `docs/site/`. One page per concept; the code on the page follows one language choice made in the top bar and remembered in the browser.
- The code lives in real files (`snippets/<lang>/<name>.<ext>`), never pasted into a page. `scripts/prepare.py check` fails when a snippet exists in one language only.
- The language list is `docs/site/languages.json` only.
- Text that applies to one language goes in `<Only lang="...">`; the table "Where the two differ" on each page records the real API differences.

## Rejected alternatives

- **MkDocs Material.** No Node, and synced content tabs exist, but there is no site-wide switch that also changes prose without custom JavaScript.
- **Keep growing the README.** It already has 604 lines and cannot show both languages at once.

## Consequences

- Node joins the toolchain for the docs only. `.github/workflows/docs.yml` builds the site: the orchestrator calls it on every push to master and on tags, after the `cmake` job has passed (informational, like autobahn: `release-wheels` does not wait for it), and pull requests run it when `docs/site/` changes. Nobody needs Node to build the library. The workflow does not publish.
- Not verified: the site build, the type check, the dark theme, keyboard use, and that the snippet harness itself works (it was syntax-checked only). The Docusaurus 3.5.2 and React 18.3.1 versions exist (the lock file resolved), but nothing was built with them.
- Every C++ claim on a page was read from `include/httpp/*.hpp` and `tests/`; every Python claim from the README and `examples/python/`. The C++ columns of the "Where the two differ" tables are the part to review first.
- `docs/site/scripts/test_snippets.py` builds the C++ snippets (with the CMake recipe from the install page) and runs all of them against the installed package, in the docs workflow. It lives outside ctest on purpose: it needs the installed wheel, and it must not make the library's own build depend on the docs.

## Related

ADR-0014, ADR-0016, ADR-0017
