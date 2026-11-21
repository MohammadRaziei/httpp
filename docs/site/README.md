# httpp docs site

One page per concept. The code on every page switches between Python and C++ with the control in the top bar.

## Run

```bash
cd docs/site
npm install
npm start          # dev server; runs scripts/prepare.py first
npm run build      # static site in build/
```

`scripts/prepare.py` needs only the Python standard library. It builds `src/generated/snippets.json` from the files in `../../examples/<topic>/<lang>/main.<ext>` and fails when an example exists in one language only or has no `README.md`.

## Test the examples

```bash
pip install .                                      # from the repository root
python -m pytest tests/python/test_examples.py     # python and cpp (cpp needs CMake; it is skipped without)
```

It runs every example on a free port and compares the result with what its page says; the C++ ones are built with the CMake recipe of `docs/install.mdx`. A new example needs a check in that file: a server example in `SERVERS`, an example that talks to one in `CLIENTS`.

## Add a lesson

1. Add `examples/<topic>/python/main.py`, `examples/<topic>/cpp/main.cpp` and `examples/<topic>/README.md`, where `<topic>` is the name of the page with `_` for `-` (`serve-directory.mdx` is `serve_directory`). Same job in both languages, listening on port 8000, plus a check in `tests/python/test_examples.py`. The README says what it does, how to run it and links to the page; it does not repeat the code. More files in the topic (`examples/<topic>/python/verified.py`, ...) are extras that the page shows below the main one with `<Lang name="<topic>" file="verified" />`; an extra may exist in one language only.
2. Add `docs/<area>/<page>.mdx`, use `<Lang name="<topic>" />` for the code and `<Only lang="cpp">...</Only>` for text that applies to one language. Do not put a heading inside `<Only>`: the table of contents would list it for the other language too.
3. Add the page id to `sidebars.ts`.

## Add a language

One line in `languages.json` and a `<lang>/main.<ext>` file in each topic of `examples/`.

## Publishing

`.github/workflows/pages.yml` deploys the site to https://mohammadraziei.github.io/httpp/ (ADR-0060) on a push to master that touches `docs/site/` or the benchmark JSON.
Set Settings, Pages, Source: "GitHub Actions" once, or the address keeps answering 404. Locally the site is at http://localhost:3000/httpp/.

## The Benchmarks page

`docs/benchmarks.mdx` draws the JSON that `benchmarks/suite.py` writes (ADR-0063) with `src/components/Bench.tsx` (chart.js). `scripts/prepare.py bench` copies the committed
`benchmarks/results/full/` (`system.json` and one file per runtime) into one `src/generated/bench.json`; with none, the page says there are no results yet.

## Not done yet

- No `ApiLink`, because there is no Doxygen or Sphinx reference to link into.
