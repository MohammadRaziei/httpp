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

## Add a lesson

1. Add `snippets/python/<name>.py` and `snippets/cpp/<name>.cpp`. Same name, same job.
2. Add `docs/<area>/<page>.mdx`, use `<Lang name="<name>" />` for the code and `<Only lang="cpp">...</Only>` for text that applies to one language.
3. Add the page id to `sidebars.ts`.

## Add a language

One line in `languages.json` and one folder in `snippets/`.

## Not done yet

- The snippets are not compiled or run by ctest. Today they are only checked for presence in both languages.
- No `ApiLink`, because there is no Doxygen or Sphinx reference to link into.
- No benchmark component, because no benchmark JSON is committed.
