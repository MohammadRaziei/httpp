# httpp docs site

One page per concept. The code on every page switches between Python and C++ with the control in the top bar.

## Run

```bash
cd docs/site
npm install
npm start          # dev server; runs scripts/prepare.py first
npm run build      # static site in build/
```

`scripts/prepare.py` needs only the Python standard library. It builds `src/generated/snippets.json` from the files in `snippets/` and fails when a snippet exists in one language only.

## Test the snippets

```bash
pip install .                               # from the repository root
python3 docs/site/scripts/test_snippets.py  # python, cpp, or both (default)
```

It runs every snippet on a free port and compares the result with what its page says; the C++ ones are built with the CMake recipe of `docs/install.mdx`. A new snippet needs a check in that script: a server snippet in `SERVERS`, a client snippet in `CLIENTS`.

## Add a lesson

1. Add `snippets/python/<name>.py` and `snippets/cpp/<name>.cpp`. Same name, same job, listening on port 8000, plus a check in `scripts/test_snippets.py`.
2. Add `docs/<area>/<page>.mdx`, use `<Lang name="<name>" />` for the code and `<Only lang="cpp">...</Only>` for text that applies to one language.
3. Add the page id to `sidebars.ts`.

## Add a language

One line in `languages.json` and one folder in `snippets/`.

## Publishing

`.github/workflows/pages.yml` deploys the site to https://mohammadraziei.github.io/httpp/ (ADR-0064) on a push to master that touches `docs/site/` or the benchmark JSON.
Set Settings, Pages, Source: "GitHub Actions" once, or the address keeps answering 404. Locally the site is at http://localhost:3000/httpp/.

## The Benchmarks page

`docs/benchmarks.mdx` draws the JSON that `benchmarks/suite.py` writes (ADR-0063) with `src/components/Bench.tsx` (chart.js). `scripts/prepare.py bench` copies the committed
`benchmarks/results/full/` (`system.json` and one file per runtime) into one `src/generated/bench.json`; with none, the page says there are no results yet.

## Not done yet

- No `ApiLink`, because there is no Doxygen or Sphinx reference to link into.
