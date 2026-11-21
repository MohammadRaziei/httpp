# ADR-0032: The file browser and the directory server: read-only browsing, a builder class, a sandbox for html and svg, hidden names and file times

- **Status:** accepted (merged ADR; each part keeps its own status line)
- **Merged on:** 2026-11-21, at the request of the project owner: the decisions of one subject were spread over 4 ADRs. The text of every part is kept, as a section below, in the order it was decided.

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history. Where a part and this summary disagree, this summary wins.

- **Read-only browser over a folder.** Our own one-file UI (no dependency, embedded compressed like Swagger UI); routes under the mount base: the page, `api/list?path=` (JSON) and `raw/<path>`. The root is a ceiling (`..`, absolute paths, backslash, NUL and symlinks out are refused with the same 404), names are written to the DOM with `textContent`, and the page carries a strict Content-Security-Policy. `raw/` is served by the `directory` mount with `directory_options`.
- **It is a builder class,** `httpp::file_browser(root)` with chained setters (title, theme, accent, search, show_hidden, max_entries, layout, sort_by; the names follow ADR-0092) and `mount(server, base)`. `theme_mode`, `view_mode` and `sort_key` are enums; values are checked where they are given. The old free function `mount_file_browser` and `file_browser_options` are removed.
- **Safe mode is off by default.** html and svg are shown: an svg as an `<img>` in the dialog, html and svg from "Open raw" inline under `Content-Security-Policy: sandbox; default-src 'none'; ...`, which stops scripts and requests to other origins. Safe mode (`--safe`) makes them downloads again, like xml, js and unknown types.
- **`httpp server` hides names that start with a dot** unless `--hidden` (the library default of `directory` is unchanged), `--no-dates` removes file times everywhere they leave the server (`mtime` of `api/list`, `Last-Modified`, `ETag`, ZIP entry dates), and both commands print a small banner.
- The page itself grew later (media player, preview dialog, file kinds); the last section of the first part describes it.

## Parts
### A read-only file browser

- **Status:** accepted (C++ core, Python wrapper and `httpp browse` done); the C++ API shape (a free function and an options struct) was replaced by the part "The file browser is a builder class with enums"; the page itself grew later (media player, preview dialog, file kinds), see the last section
- **Source:** `include/httpp/file_browser.hpp`, `src/core/file_browser.cpp`, `src/assets/file_browser.html`, `server::directory_options` in `src/core/server.cpp`, `tests/cpp/test_file_browser.cpp`

#### Context

The owner wants to point httpp at a folder and browse it (list, navigate, preview, download) from a browser, with options (theme, accent, search, hidden files, limits). No write
operations. jsfb (Apache-2.0, a 30 KB script plus 23 KB of CSS, rename and delete in its menu) was the inspiration. Facts that shaped the design: the router has no wildcard, so
a deep path needs the `serve_directory` mount; nothing read directories before; `std::filesystem` is banned in `src/core` (macOS 10.13 wheels); there is no ETag or
Last-Modified; the server has no authentication of its own.

#### Decision

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

#### Rejected alternatives

- Embedding jsfb: its menu offers rename and delete, it would need patching, and a third-party minified bundle is harder to audit than 200 readable lines.
- A recursive server-side search now: unbounded work per request without limits that need their own design.
- `std::filesystem` for listing on all platforms: banned in `src/core` for the macOS 10.13 wheels (the Windows branch uses it, where that restriction does not apply).
- Serving `raw/` inline for everything and relying on the CSP: a download header is simpler and does not depend on every browser honouring the policy.

#### Consequences

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

#### Later changes to the page (2026-11-14)

Requested by the project owner after using the page; the details are Claude's. All of it is in `src/assets/file_browser.html`: still one file with inline CSS, JS and SVG, no dependency, no new request, so the Content-Security-Policy above is unchanged and so is the C++ and Python API. The page grew from about 12 KB to about 28 KB before compression (it is embedded compressed, ADR-0031).

- **Media player.** The preview used `<audio controls>` and `<video controls>`, whose controls ignore the page's theme and accent and differ between browsers. The media element stays without `controls` and the page draws the bar from its own CSS variables. Controls: play and pause, a seek bar with the buffered range (the position is committed on release, so a drag does not send a range request per pixel), time, mute and volume (kept in `localStorage`), speed (a button cycling 1, 1.25, 1.5, 2, 0.75), full screen for video. Keys: space, left and right (5 s), up and down (volume), `m`, `f`. Video: a click toggles play, a double click toggles full screen, the bar hides after 2.5 s without movement. Previous and next appear when the folder has more than one file of that kind (in the order shown, filter applied); a file that ends starts the next; previous restarts the current file after 3 s. Closing stops the element and removes its `src`, so a large file stops downloading. A webm file without a duration reports `Infinity`; the player seeks far past the end once so the browser finds it. A file that cannot be decoded shows a message that points to Download and Open raw.
- **Preview dialog.** Download, Open raw and Close are three icon buttons at the top right with tooltips drawn in CSS from `data-tip` (shown after a short delay, also on keyboard focus). The name is the title and the size a quiet second line. The dialog is larger (`min(60rem, 94vw)`, preview up to 66vh), with a larger radius, a shadow and a blurred backdrop. It opens and closes with a short animation (about 0.2 s in, 0.16 s out). The Close button, the Escape key (the `cancel` event is intercepted) and a click on the backdrop all go through one function that adds a class and calls `close()` after the animation time, so the `close` event still stops the media. A click closes only when the press and the release are both on the backdrop: the event must be aimed at the dialog element itself and lie outside its box. The first version tested only the coordinates, and a full screen video, which covers the screen far outside the dialog box, was taken for the backdrop and closed the dialog on the first click. With `prefers-reduced-motion` there is no animation.
- **File kinds and a Type column.** Every file used the same grey icon. The kind comes from the extension: image, audio, video, PDF, archive, text (the existing text list), other; a folder is its own kind. Each kind has an inline SVG icon and a fixed colour that reads on both themes (folders keep the accent). The kind lists are wider than the preview lists: a `flac`, `mkv` or `svg` gets the audio, video or image icon even though the page may not preview it, because the icon says what the file is. The list view has a Type column between Name and Size: the kind word, for other files the extension in capitals (or `File`). The header sorts by it (name breaks ties, folders stay first). The sort key `type` exists only in the page; the `sort` option of the C++ and Python API is unchanged. The column is not drawn in the grid view (the icon carries it) nor on narrow screens.
- **ZIP download of a selection (added later, `set_zip`, off in the library, on in `httpp browse`).** Asked for by the project owner: pick files or folders and get them in one archive. The goal is bundling, not smaller files, so **nothing is compressed**. Each file is written as deflate "stored" blocks (method 8, 5 bytes of framing per 64 KB) with the CRC-32 and sizes in a data descriptor after the data (`src/core/internal/zip_stream.hpp`, with an incremental `crc32_update`): the archive is produced in one pass over each file, streamed with `response::stream`, so memory stays small (7 MB resident after a 600 MB download) and a stored block, unlike method 0 with a descriptor, is a legal deflate stream that ends by itself, which extractors that read from the front (not only from the central directory) need. Names are UTF-8 (flag bit 11). Zip64 is used per entry from 4 GiB minus 4 MiB, for offsets and a central directory from 4 GiB, and for 65535 entries or more. The sizes are those of the plan, made before the first byte (a file that ends early drops the connection; one that grew is cut). `GET|POST base/api/zip` takes `path` and repeated `name` (a form for POST, so a long selection fits); no `name` means the whole folder. The plan walks the tree with the same rules as the listing (hidden names unless `show_hidden`, names must be UTF-8, a link is followed only if it resolves inside the root, and a link to a folder only when it was chosen by name, never deeper, because it could loop; depth 64) and answers `400`, `404` or `413` (`set_max_zip_entries`, default 50000) before sending anything. The page uses `form-action 'self'` in its CSP only when zip is on. It asks with `fetch` first and drops the answer, so a refusal is shown in the page, then submits a form so the browser saves the stream to disk without holding it in memory. The response is chunked, so the browser shows no total. Off by default in the library because a request can read a lot of disk; `httpp browse`, a personal tool, has it on.
- **Selection, right-click and credit (page only).** Checkboxes (Shift range, Space, Ctrl+A), a rectangle drawn with the mouse (a press on empty space, or a press on a row that moves more than 6 px; Ctrl or Shift adds; touch is left to scrolling; the page scrolls near the edges), and a bar with Download ZIP, Copy names and Clear. A right-click opens the page's own menu (entry, many selected, empty space) with keyboard support; the filter box, the column headers and the preview dialog keep the browser's menu. Copying uses `navigator.clipboard` on https and localhost and `execCommand` elsewhere (a plain http address on the network has no clipboard API). A small "Powered by httpp" link to the repository sits at the bottom (`set_powered_by(false)` removes it); the page makes no request to GitHub until it is clicked.
- **Rejected:** a media library such as Plyr or Video.js (a dependency of tens of KB against a page that can be audited in one read); styling the native controls (only a few pseudo-elements, different in each engine); native `title` tooltips (slow, unstyled, not shown on touch or focus); a setting to turn click-outside off (the Close button and Escape stay either way); closing on `animationend` (the backdrop animation reports too; a timeout that matches the CSS is simpler); a playlist panel; thumbnails (a request and a decode per file, and a decision about large folders); reading file contents to find the kind (one more request per file, and the extension is what the preview uses).
- **Verified:** the script runs in jsdom with the media API stubbed: the player is built for audio and for video without a `controls` attribute, play and pause, the mute key, next, speed, closing clears the player; the three icon buttons have the right links and tooltips and the old text buttons are gone; close by button, by backdrop click and by Escape each go through the closing class and then close; a click on the player far outside the dialog box (what full screen looks like) does not close it, and that check fails on the first version; a list of 13 entries gets the right kind, label and icon for each (eight kinds give eight different icons), sorting by type works in both directions, the `..` row has an empty type, the grid view renders; no script error. Three C++ tests (`the_page_ships_its_own_media_player_and_no_native_controls`, `the_preview_dialog_has_icon_buttons_with_tooltips`, `the_list_has_a_type_column_and_typed_icons`) are written to check the shipped page, but were not compiled or run here. The owner opened the page in a browser, found the full screen bug and confirmed it fixed.
- **Verified (ZIP):** the writer alone, against Python's `zipfile` (`testzip`) and `unzip -t`: plain, Zip64 per entry, 64-bit offsets, both, 70000 entries (Zip64 end records), Persian names, folders, an empty file, a file of more than one block; the built `httpp browse` with GET and POST, the refusals (`..`, a name with `/`, hidden, missing, a link out, a file as folder), 600 MB in 2.9 s and 7 MB resident, a client that hangs up mid-download and a server that keeps answering; the page in jsdom (checkboxes, Shift range, select all, Ctrl+click, Space, the bar, the fetch then form flow, a refusal shown in the toast, the menu for a file, a folder, many selected and empty space with arrows, Home, End, Esc and an outside click, Esc not clearing a selection, the rectangle with a stubbed layout, the 6 px threshold, Ctrl-drag, touch ignored, a drag not opening the row under the pointer, Ctrl+A, the credit link and its off switch).
- **Not verified (ZIP and selection):** every archive was read by `zipfile` and `unzip`, not by Windows Explorer, macOS Archive Utility, 7-Zip or Android; a real file above 4 GiB (the 64-bit paths were reached by lowering the thresholds); the page in a real browser, so the look of the menu, the rectangle on a real layout and its auto-scroll, `execCommand` copy on plain http, and how browsers show a download with no total; the Python tests and the CLI flags in the Python test suite were written but the Python extension was not built here.
- **Not verified:** the rest of the behaviour in a real browser: the look of the dialog, tooltips, icons and colours (the PDF and archive icons at small size, the light theme), animation timing and the backdrop blur, Escape pressed twice quickly (some browsers treat it as a forced close), pointer dragging on the sliders, the idle bar, the duration fix, keyboard focus inside the dialog, the width of the Type column against long names, the narrow-screen layout, touch devices, a 4 GB file. Names that mix right-to-left and left-to-right text are still shown in the order the Unicode algorithm gives them.

### The file browser is a builder class with enums

- **Status:** accepted; amends the part "A read-only file browser"
- **Decided by:** the project owner ("this is not principled: it should be OOP, `file_browser(...).set_...().mount()`, and the view must be an enum"); the details are Claude's
- **Source:** `include/httpp/file_browser.hpp`, `src/core/file_browser.cpp`, `src/cli/httpp_cli.cpp`, `src/bindings/python/cy/server.pxi`, `tests/cpp/test_file_browser.cpp`

#### Context

the part "A read-only file browser" shipped the browser as a free function `mount_file_browser(server&, base, root, file_browser_options)`. The options were a plain struct whose theme, view and sort were strings ("auto", "grid", "size"): a typo compiled, was checked only when mounting, and every other part of the library (`server::set_*`, ADR-0054 `path`) is a class with chained setters instead.

#### Decision

- `httpp::file_browser(root)` with chained setters `set_title`, `set_theme`, `set_accent`, `set_search`, `set_show_hidden`, `set_max_entries`, `set_view`, `set_sort`, and `mount(server&, base = "/") const`.
- `theme_mode { system, light, dark }`, `view_mode { list, grid }`, `sort_key { name, size, date }` are `enum class` members of `file_browser`. `system` is the old "auto" (`auto` is a keyword); the page still receives the string "auto", so the HTML is unchanged.
- Values are checked where they are given: `set_accent` (`#rrggbb` or empty) and `set_max_entries` (above 0) throw `std::invalid_argument` at the setter and leave the browser unchanged. `mount` checks what depends on the world: `base` starts with '/', `root` is a folder, `root` is not the root of the file system. The root is not checked in the constructor, because the folder may be created before mounting.
- `file_browser_options` and `mount_file_browser` are removed, not kept as deprecated aliases (version 0.x, as `httpp::fs` was removed in ADR-0054). Listed as a change in the CHANGELOG.
- Python mirrors it: `httpp.FileBrowser(root, **keywords)` with the same chained `set_*` methods (each returns the browser) and `mount(server, base="/")`; the enums are `FileBrowser.Theme` (`SYSTEM`, `LIGHT`, `DARK`), `FileBrowser.View` (`LIST`, `GRID`) and `FileBrowser.Sort` (`NAME`, `SIZE`, `DATE`), real `enum.Enum` types, and an enum setter given a string or another enum raises `TypeError`. Constructor keywords are the setters without `set_`, so both forms exist as on `Server` and the clients. `Server.mount_file_browser(base, root, **options)` is removed: one way to do it in both languages. The numbers behind the Python enums are checked against the C++ enums by `static_assert` in `cy/support.pxi`, so reordering a C++ enum breaks the build instead of the meaning. The CLI (`httpp browse`) keeps its `--theme/--view/--sort` choices and maps them to the enums.
- Export: the member functions carry `HTTPP_API`, not the class, so the private `std::string` members need no DLL interface (ADR-0011 exported classes because they had to be derived from).

#### Consequences

- A wrong value cannot be written in C++ for theme, view and sort (it does not compile); a bad accent or `max_entries` fails at the line that sets it.
- Breaking for users of 0.8.0 and 0.8.1: C++ `httpp::mount_file_browser(s, "/files", root, options)` becomes `httpp::file_browser(root).set_...().mount(s, "/files")`; Python `app.mount_file_browser("/files", root, theme="dark")` becomes `FileBrowser(root, theme=FileBrowser.Theme.DARK).mount(app, "/files")`. The CLI is unchanged.
- The browser object can be mounted more than once.

#### Verification

Sandbox, Linux, Release. C++ suite 524 passed (16 of them the file browser; three new: the setters chain, a refused value changes nothing, the defaults reach the page). Python 3.14t, wheel built from the final tree: 868 passed, 4 skipped, including `test_file_browser.py` (rewritten: enums, keywords, chaining, mounting twice, a bad value at the setter changes nothing, `TypeError` for a string where an enum is expected, `Server.mount_file_browser` gone), the parity test (the new class is mapped to `httpp.FileBrowser` and every C++ setter has a Python method) and the CLI tests.
Not verified: sanitizers on the new code, Windows (MSVC and the method-level `HTTPP_API`), macOS, Python 3.13 and 3.14 with a GIL for this change.

### The file browser shows html and svg inside a sandbox; safe mode turns that off

- **Status:** accepted; amends the part "A read-only file browser" (html and svg were always downloads) and the part "The file browser is a builder class with enums"
- **Decided by:** the project owner ("a method on the file browser class that controls it; html as text in the dialog, but really rendered when \"Open raw\" opens it, no iframe; unsafe is the default and `--safe` turns safe mode on"); the details are Claude's
- **Source:** `include/httpp/file_browser.hpp`, `include/httpp/server.hpp`, `src/core/file_browser.cpp`, `src/core/server.cpp`, `src/assets/file_browser.html`, `src/cli/httpp_cli.cpp`, `src/bindings/python/cy/{file_browser,directory,support}.pxi`

#### Context

the part "A read-only file browser" made every type that can run code in the page's origin (html, svg, xml, js, unknown) a download, and the dialog has no preview for them. That is the right default for a folder nobody vouches for, but it is also why an svg showed "No preview for this file type" and why "Open raw" saved the file instead of opening it. Someone who browses their own folder wants to see both.

#### Decision

- `file_browser::set_safe_mode(bool on)`, default `false` (this changes the behaviour of the part "A read-only file browser": html and svg were always downloads). With `false`:
  - **Dialog:** an svg is shown as an image (`<img>`: scripts and outside requests never run there). An html file stays in the text preview it always had; there is no frame, so the page's own policy is unchanged.
  - **"Open raw":** html and svg are sent inline, not as an attachment, with `Content-Security-Policy: sandbox; default-src 'none'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; font-src 'self' data:; media-src 'self'; base-uri 'none'; form-action 'none'`. Without `allow-scripts` the sandbox stops every script, and `default-src 'none'` stops every request to another origin, so even a hostile file cannot reach the page, the cookies or the network. Images, styles, fonts and media of the same server still load, so a plain page renders as written.
  - **Still downloads:** xml, js, every unknown type, and any file asked for with `?download=1`.
  - The server side is a separate switch, `directory::set_inline_active(bool)`, default `false`, which only has an effect together with `set_download_active(true)`. `file_browser` mounts its `/raw` folder with `set_inline_active(!safe_mode)`. A plain `directory` gets the same behaviour for its own users.
- **Caching.** The headers of a `download_active` answer depend on the mode, so (a) the ETag carries a one-letter suffix for how the file is sent (`-p` as it is, `-s` sandboxed inline, `-a` download) and (b) the answer says `Cache-Control: no-cache`, also on its 304. Without (a) a browser that cached `index.html` as a download kept the old `Content-Disposition` through every 304 after the mode changed (the validator was the same); without (b) it did not even ask while its heuristic freshness lasted (hours for an old file), so changing the mode appeared to do nothing. Found on the owner's machine and reproduced in headless Chromium; the cost is one conditional request per file view, a 304 when nothing changed. A plain `serve_directory` keeps its caching.
- `set_safe_mode(true)` is the old behaviour: html, svg, xml, js and unknown types are only downloaded and the dialog has no preview for them.
- The page learns the mode through the config object (`"safe":true|false`); a missing key means the default, not safe.
- Python mirrors it (ADR-0016 and the part "The file browser is a builder class with enums"): `FileBrowser.set_safe_mode` and `safe_mode=`, `Directory.set_inline_active` and `inline_active=`. The CLI gets `httpp browse --safe`. The parity test needs no change because the names match.
- Names are the owner's: "safe mode", and the CLI flag is `--safe` because the default is the opposite. The default is not as reckless as the word "unsafe" suggests: what is shown is always behind the sandbox policy, which is what makes it acceptable as a default. A project that wants the old rule everywhere calls `set_safe_mode(true)`.

#### Rejected alternatives

- **A setter only for svg.** html would stay a download, and the owner asked for rendered html.
- **Showing html and svg without a policy** (just dropping the attachment header). A script in such a file would run in the origin of the browser page and could call `/raw` and `/api/list` as the viewer. Rejected: the sandbox is what makes the switch acceptable.
- **A template library (mustache) for the page.** The page needs two placeholders and one more key in a JSON object; a dependency adds nothing.
- **Rendering html in the dialog (a sandboxed `<iframe>`).** Rejected by the owner: the dialog shows html as text, and the rendered page is what "Open raw" is for. It would also have needed `frame-src` in the page's policy.
- **Also showing xml and js inline.** They have no useful rendering and the same risk; they stay downloads.

#### Consequences

- The default is exactly the old behaviour; the existing tests for active types pass unchanged.
- With safe mode off, a rendered html page cannot run its own scripts, so a page that needs JavaScript shows without it. That is deliberate. A form in it cannot submit (`form-action 'none'`) and nothing it links to is fetched by the page itself (`default-src 'none'`).
- A relative stylesheet or image next to the html loads, but is subject to the same download rules as any other file in `/raw` (a `.css` is served with `attachment`, which browsers ignore for sub-resources).

#### Verification

Sandbox, Linux, Release, Python 3.12, headless Chromium (from npm). C++ suite: 550 passed (new: the ETag and `Cache-Control` of a `download_active` answer follow the mode; by default html and svg are inline behind the sandbox and `?download=1` and the other active types still download; `set_safe_mode(true)` makes html and svg downloads too; `directory::set_inline_active` with and without `set_download_active`; the older "active types are downloads" tests now say `set_safe_mode(true)`). Python wheel layout built from the final tree: 615 passed, 7 skipped (after rebasing onto upstream's logging/CLI commit c326808, which merged without a textual conflict), including the new `test_file_browser.py` and `test_builders.py` cases, the parity test and the CLI test for `httpp browse` with and without `--safe`.
Checked by hand in headless Chromium against `httpp browse` (no flag): in the dialog an html file is text and an svg is an image; "Open raw" opens a rendered page (the h1 is there, the script did not run) and a rendered svg, with no download; with `--safe` the same URL is a download. A browser profile that had loaded the file in one mode and then met a server in the other mode showed the old behaviour until `Cache-Control: no-cache` and the mode-dependent ETag were added (see Caching).
Not verified: Windows, macOS, sanitizers, other browsers.

### File times can be switched off, `httpp server` hides dot names, and the two commands print a small banner

- **Status:** accepted; amends ADR-0066 (`directory` settings), the part "A read-only file browser" (file browser) and ADR-0041 (the command line)
- **Decided by:** the project owner ("add `--hidden` and `--no-dates` to both commands, and a small ascii art for httpp when these two commands are called"); the details are Claude's
- **Source:** `include/httpp/{server,file_browser}.hpp`, `src/core/{server,file_browser}.cpp`, `src/assets/file_browser.html`, `src/cli/httpp_cli.cpp`, `src/bindings/python/cy/{directory,file_browser,support}.pxi`, `tests/cpp/test_show_dates.cpp`

#### Context

Two things were found while writing the documentation of `httpp server` and `httpp browse`:

1. `httpp server DIR` listened on `0.0.0.0` and served names that start with a dot, so `httpp server .` inside a project handed `.env` and `.git` to the whole network. `httpp browse` already hid them (`--hidden` to show).
2. The modified time of every file left the server in four places even when the page did not show it: the `mtime` of `api/list`, the `Last-Modified` header, the `ETag` (it holds the time of the file as hex) and the date of each entry of a ZIP. A viewer could read when files were edited with `curl`.

#### Decision

- **`httpp server --hidden`** and the default flipped: names that start with a dot answer 404 unless `--hidden`. The command now builds `directory(dir).set_show_hidden(hidden).set_show_dates(!no_dates)` and mounts it at `/` (what `serve_directory("/", dir)` did, with these two settings). The library default of `directory` (dot names served) is **not** changed: it is documented and the change would break programs.
- **`directory::set_show_dates(bool)`**, default `true`. With `false` the answer has no `Last-Modified` and no `ETag`, so no conditional request can succeed (no 304, no 412) and `If-Range` cannot be checked: a range request that carries one gets the whole file (RFC 9110 section 13.1.5). The answer says `Cache-Control: no-cache`. A plain `Range` still gets 206.
- **`file_browser::set_show_dates(bool)`**, default `true`. With `false`: `api/list` has no `mtime`; the page config carries `"dates":false`, so the page drops the Modified column and the sort by date (a first sort of `date` falls back to `name`); the `/raw` folder is mounted with `set_show_dates(false)`; every entry of a ZIP is stamped with the earliest DOS date, 1980-01-01 00:00 (a time of 0 already maps there).
- **`--no-dates`** on `httpp server` and `httpp browse`; Python `Directory(show_dates=)`, `FileBrowser(show_dates=)` and `set_show_dates`. The names match, so the parity test needs no change.
- **Banner.** Both commands print five lines of ASCII art and the version to **stderr**, only when stderr is a terminal, and not with `--log-level off` or the new `--no-banner`. A pipe, a log file and a CI run never see it, and the `Serving ...` / `Browsing ...` line on stdout is unchanged, so scripts that read it keep working.
- Not tied to `--safe`: `--safe` is about code that could run in the viewer's browser; file times are a privacy setting, and a user who turns one on should not lose the other.

#### Rejected alternatives

- **`httpp server --no-hidden`.** It would keep the dangerous default and make the two commands opposites (`browse` has `--hidden`).
- **Hiding only the Modified column.** The times stay in the JSON, the headers and the ZIP; it would look private and not be.
- **An ETag from the size alone.** An edit that keeps the size would keep the same validator and a browser would show a stale file. No validator plus `no-cache` is simpler and correct, at the cost of no 304.
- **The banner on stdout, or always.** It would break scripts that parse the first line and fill CI logs.
- **A random or empty ETag when dates are off.** A validator that cannot be checked is worse than none.

#### Consequences

- **Breaking for `httpp server` (alpha):** dot names answer 404. `/.well-known/` (ACME challenges) needs `--hidden`. The documentation says so.
- With dates off, repeat requests for a large file are full transfers (no 304); `Range` and resumable downloads still work, except that a client's `If-Range` falls back to the whole file.
- `--no-dates` does not hide the *size* of files or the `Date` header of the response (the server's own clock).

#### Verification

Sandbox, Linux, Release, Python 3.12. C++: `tests/cpp/test_show_dates.cpp` (6 new: validators and 304 by default; none with dates off, `If-None-Match: *` and `If-Modified-Since` answer 200; `Range` is 206 and `Range` with `If-Range` is 200; the setters chain; the browser lists `mtime` and the page says `"dates":true` by default; with dates off no `mtime`, no `Last-Modified` or `ETag` on `/raw`, and the first ZIP entry has DOS time 0 and date `0x0021`), plus the 42 existing file browser and conditional-request tests, unchanged. Python wheel built from this tree: `test_builders.py` and `test_file_browser.py` 58 passed (new: `show_dates` on both classes, as setter and as keyword, the ZIP dates read back with `zipfile`), `test_cli.py` 99 passed (new: dot names hidden and `--hidden` serving `.env` and `.well-known/acme`; `--no-dates` removes `ETag` and `Last-Modified`; the banner appears on a pseudo-terminal and not on a pipe, with `--no-banner` or with `--log-level off`), and the parity, server-feature, embed and example tests. Not verified: Windows and macOS (the terminal check there is `_isatty` / `isatty` and was not run), and the page in a browser other than headless Chromium.
