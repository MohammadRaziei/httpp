# ADR-0032: A read-only file browser

- **Status:** accepted (C++ core, Python wrapper and `httpp browse` done); the C++ API shape (a free function and an options struct) was replaced by ADR-0065
- **Source:** `include/httpp/file_browser.hpp`, `src/core/file_browser.cpp`, `src/assets/file_browser.html`, `server::directory_options` in `src/core/server.cpp`, `tests/cpp/test_file_browser.cpp`

## Context

The owner wants to point httpp at a folder and browse it (list, navigate, preview, download) from a browser, with options (theme, accent, search, hidden files, limits). No write
operations. jsfb (Apache-2.0, a 30 KB script plus 23 KB of CSS, rename and delete in its menu) was the inspiration. Facts that shaped the design: the router has no wildcard, so
a deep path needs the `serve_directory` mount; nothing read directories before; `std::filesystem` is banned in `src/core` (macOS 10.13 wheels); there is no ETag or
Last-Modified; the server has no authentication of its own.

## Decision

- **Our own small UI**, one HTML file (CSS and JS inline, inline SVG icons, no dependency, no network except the server's own two endpoints, about 12 KB), embedded with
  `embed_files(... COMPRESS)` like Swagger UI (ADR-0031). The page is a template: `mount_file_browser` inflates it once and fills in the title (HTML-escaped) and the options (a JSON
  object in which `<`, `>` and `&` are escaped, so a title or base cannot close the script). The root path on disk is never in the page. All names are written to the DOM with
  `textContent`. A strict `Content-Security-Policy` (`default-src 'none'`, inline script and style only, own origin for fetch, images and media, no framing) is sent with it.
- **`httpp::mount_file_browser(server&, base, root, file_browser_options)`**, a free function like `mount_swagger_ui` (the member proposed earlier was dropped: it would add a
  public method to `server` for something that needs only public API plus the one extension below). Routes under `base`: the page (`base`, `base/`), `api/list?path=` (JSON) and `raw/<path>`.
- **`server::directory_options` and a third `serve_directory` overload** (`index`, `hidden`, `download_active`; the defaults are the old behaviour, tested). `raw/` is a mount with
  `index=false` (a folder is never answered with its `index.html`), `hidden=show_hidden` (a name with a leading '.' is a 404) and `download_active=true`: `X-Content-Type-Options: nosniff`, and
  everything but plain text, JSON, CSV, Markdown, PDF, common images (not SVG), audio and video is sent with `Content-Disposition: attachment; filename=...; filename*=UTF-8''...`, and so is
  anything asked for with `?download=1`. Range, HEAD, 206 and 416 come from the existing static code. A NUL in a static path is now refused for every mount.
- **The served root is a ceiling.** In the list API: `..`, `.`, a backslash or NUL is a 400; a hidden name, a missing path, a file, a symlink that leaves the root and an absolute-looking path
  that is simply not inside the root are all the same 404 with the same body. The page shows no `..` row at the root. Entries that are symlinks pointing out of the root, broken links, sockets, pipes and
  devices are not listed; names that are not valid UTF-8 are left out (the JSON must stay valid). The root `/` (and the root of a drive) is refused when mounting; so is a root that is not a folder.
- **Listing runs on a pool thread** (an ordinary `get` route), with `opendir` and `readdir` on POSIX, and stops after `max_entries` (default 5000) kept entries; the answer says `truncated` and `limit`.
  Order inside a truncated folder is whatever the file system returned; what is returned is sorted (folders first, then name, case-folded for ASCII).
- **No write route exists.** POST, PUT, PATCH and DELETE find nothing (404 or 405) and the tests check that the files are untouched.
- **Authentication is not in the browser.** The server's `before` hook runs first for every route (page, list and raw), so it is the place for it; the docs of `mount_file_browser` say so.
- **`search` is a filter of the open folder, in the browser.** A recursive search is not in v1: it needs depth, count and time limits to be safe against a denial of service. No ETag or 304 in v1.

## Rejected alternatives

- Embedding jsfb: its menu offers rename and delete, it would need patching, and a third-party minified bundle is harder to audit than 200 readable lines.
- A recursive server-side search now: unbounded work per request without limits that need their own design.
- `std::filesystem` for listing on all platforms: banned in `src/core` for the macOS 10.13 wheels (the Windows branch uses it, where that restriction does not apply).
- Serving `raw/` inline for everything and relying on the CSP: a download header is simpler and does not depend on every browser honouring the policy.

## Consequences

- Verified: 15 new C++ tests (listing, hidden files, 26 traversal attempts including encoded and backslash forms and symlinks out, equal 404 bodies, write methods, ranges and HEAD, download headers, folders and
  `index.html`, `max_entries`, the page and its escaping, mounting at `/`, bad options, and that the old `serve_directory` still serves dotfiles and `index.html`); the whole C++ suite (407 tests) passes.
  The page was opened in headless Chromium: no console error, list and grid views, navigation, an image preview, a text preview, the filter, light and dark.
- **Not verified:** Windows (the branch is written, never compiled or run: path rules for `:` and reserved names are untested), macOS, a TSan or ASan run of the new code, behaviour with very large folders
  (nothing measured beyond the tests), browsers other than Chromium, `fs.protected_symlinks`-style races (a link swapped between the check and the open is the classic time-of-check gap of `serve_directory`
  too and is not closed here).
- **Python (added later):** `Server.mount_file_browser(base, root, *, title, theme, accent, search, show_hidden, max_entries, view, sort)` in `httpp_cy.pyx`, a thin pass-through (the options and their
  validation live in C++; a bad option or root raises `ValueError`, `max_entries < 1` is checked in the wrapper because a negative value would wrap to a huge `size_t`). `httpp browse [DIRECTORY]` in `__main__.py`
  (`--host` default 127.0.0.1, `--port`, `--title`, `--theme`, `--accent`, `--no-search`, `--hidden`, `--max-entries`, `--view`, `--sort`). The parity test knows the new header.
  Verified: `tests/python/test_file_browser.py` (12 tests: listing, options, bad options, write methods, the command started as a process, bad folder and bad accent) and the whole Python suite on 3.14 with
  the GIL (665 passed, 4 skipped). Later runs (2026-11-05): the whole C++ suite (407) is clean under AddressSanitizer (memory errors; leak detection reports only async client objects alive at exit, none from the file browser) and under ThreadSanitizer;
  the Python suite passes on 3.14t (664 passed, 5 skipped) and 3.13 (665 passed, 4 skipped). Still not run: Windows, macOS, sanitizers on the Python extension.
