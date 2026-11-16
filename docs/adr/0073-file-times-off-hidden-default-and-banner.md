# ADR-0073: File times can be switched off, `httpp server` hides dot names, and the two commands print a small banner

- **Status:** accepted; amends ADR-0066 (`directory` settings), ADR-0032 (file browser) and ADR-0041 (the command line)
- **Decided by:** the project owner ("add `--hidden` and `--no-dates` to both commands, and a small ascii art for httpp when these two commands are called"); the details are Claude's
- **Source:** `include/httpp/{server,file_browser}.hpp`, `src/core/{server,file_browser}.cpp`, `src/assets/file_browser.html`, `src/cli/httpp_cli.cpp`, `src/bindings/python/cy/{directory,file_browser,support}.pxi`, `tests/cpp/test_show_dates.cpp`

## Context

Two things were found while writing the documentation of `httpp server` and `httpp browse`:

1. `httpp server DIR` listened on `0.0.0.0` and served names that start with a dot, so `httpp server .` inside a project handed `.env` and `.git` to the whole network. `httpp browse` already hid them (`--hidden` to show).
2. The modified time of every file left the server in four places even when the page did not show it: the `mtime` of `api/list`, the `Last-Modified` header, the `ETag` (it holds the time of the file as hex) and the date of each entry of a ZIP. A viewer could read when files were edited with `curl`.

## Decision

- **`httpp server --hidden`** and the default flipped: names that start with a dot answer 404 unless `--hidden`. The command now builds `directory(dir).set_show_hidden(hidden).set_show_dates(!no_dates)` and mounts it at `/` (what `serve_directory("/", dir)` did, with these two settings). The library default of `directory` (dot names served) is **not** changed: it is documented and the change would break programs.
- **`directory::set_show_dates(bool)`**, default `true`. With `false` the answer has no `Last-Modified` and no `ETag`, so no conditional request can succeed (no 304, no 412) and `If-Range` cannot be checked: a range request that carries one gets the whole file (RFC 9110 section 13.1.5). The answer says `Cache-Control: no-cache`. A plain `Range` still gets 206.
- **`file_browser::set_show_dates(bool)`**, default `true`. With `false`: `api/list` has no `mtime`; the page config carries `"dates":false`, so the page drops the Modified column and the sort by date (a first sort of `date` falls back to `name`); the `/raw` folder is mounted with `set_show_dates(false)`; every entry of a ZIP is stamped with the earliest DOS date, 1980-01-01 00:00 (a time of 0 already maps there).
- **`--no-dates`** on `httpp server` and `httpp browse`; Python `Directory(show_dates=)`, `FileBrowser(show_dates=)` and `set_show_dates`. The names match, so the parity test needs no change.
- **Banner.** Both commands print five lines of ASCII art and the version to **stderr**, only when stderr is a terminal, and not with `--log-level off` or the new `--no-banner`. A pipe, a log file and a CI run never see it, and the `Serving ...` / `Browsing ...` line on stdout is unchanged, so scripts that read it keep working.
- Not tied to `--safe`: `--safe` is about code that could run in the viewer's browser; file times are a privacy setting, and a user who turns one on should not lose the other.

## Rejected alternatives

- **`httpp server --no-hidden`.** It would keep the dangerous default and make the two commands opposites (`browse` has `--hidden`).
- **Hiding only the Modified column.** The times stay in the JSON, the headers and the ZIP; it would look private and not be.
- **An ETag from the size alone.** An edit that keeps the size would keep the same validator and a browser would show a stale file. No validator plus `no-cache` is simpler and correct, at the cost of no 304.
- **The banner on stdout, or always.** It would break scripts that parse the first line and fill CI logs.
- **A random or empty ETag when dates are off.** A validator that cannot be checked is worse than none.

## Consequences

- **Breaking for `httpp server` (alpha):** dot names answer 404. `/.well-known/` (ACME challenges) needs `--hidden`. The documentation says so.
- With dates off, repeat requests for a large file are full transfers (no 304); `Range` and resumable downloads still work, except that a client's `If-Range` falls back to the whole file.
- `--no-dates` does not hide the *size* of files or the `Date` header of the response (the server's own clock).

## Verification

Sandbox, Linux, Release, Python 3.12. C++: `tests/cpp/test_show_dates.cpp` (6 new: validators and 304 by default; none with dates off, `If-None-Match: *` and `If-Modified-Since` answer 200; `Range` is 206 and `Range` with `If-Range` is 200; the setters chain; the browser lists `mtime` and the page says `"dates":true` by default; with dates off no `mtime`, no `Last-Modified` or `ETag` on `/raw`, and the first ZIP entry has DOS time 0 and date `0x0021`), plus the 42 existing file browser and conditional-request tests, unchanged. Python wheel built from this tree: `test_builders.py` and `test_file_browser.py` 58 passed (new: `show_dates` on both classes, as setter and as keyword, the ZIP dates read back with `zipfile`), `test_cli.py` 99 passed (new: dot names hidden and `--hidden` serving `.env` and `.well-known/acme`; `--no-dates` removes `ETag` and `Last-Modified`; the banner appears on a pseudo-terminal and not on a pipe, with `--no-banner` or with `--log-level off`), and the parity, server-feature, embed and example tests. Not verified: Windows and macOS (the terminal check there is `_isatty` / `isatty` and was not run), and the page in a browser other than headless Chromium.
