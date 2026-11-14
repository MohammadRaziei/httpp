# ADR-0070: The file browser shows html and svg inside a sandbox; safe mode turns that off

- **Status:** accepted; amends ADR-0032 (html and svg were always downloads) and ADR-0065
- **Decided by:** the project owner ("a method on the file browser class that controls it; html as text in the dialog, but really rendered when \"Open raw\" opens it, no iframe; unsafe is the default and `--safe` turns safe mode on"); the details are Claude's
- **Source:** `include/httpp/file_browser.hpp`, `include/httpp/server.hpp`, `src/core/file_browser.cpp`, `src/core/server.cpp`, `src/assets/file_browser.html`, `src/cli/httpp_cli.cpp`, `src/bindings/python/cy/{file_browser,directory,support}.pxi`

## Context

ADR-0032 made every type that can run code in the page's origin (html, svg, xml, js, unknown) a download, and the dialog has no preview for them. That is the right default for a folder nobody vouches for, but it is also why an svg showed "No preview for this file type" and why "Open raw" saved the file instead of opening it. Someone who browses their own folder wants to see both.

## Decision

- `file_browser::set_safe_mode(bool on)`, default `false` (this changes the behaviour of ADR-0032: html and svg were always downloads). With `false`:
  - **Dialog:** an svg is shown as an image (`<img>`: scripts and outside requests never run there). An html file stays in the text preview it always had; there is no frame, so the page's own policy is unchanged.
  - **"Open raw":** html and svg are sent inline, not as an attachment, with `Content-Security-Policy: sandbox; default-src 'none'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; font-src 'self' data:; media-src 'self'; base-uri 'none'; form-action 'none'`. Without `allow-scripts` the sandbox stops every script, and `default-src 'none'` stops every request to another origin, so even a hostile file cannot reach the page, the cookies or the network. Images, styles, fonts and media of the same server still load, so a plain page renders as written.
  - **Still downloads:** xml, js, every unknown type, and any file asked for with `?download=1`.
  - The server side is a separate switch, `directory::set_inline_active(bool)`, default `false`, which only has an effect together with `set_download_active(true)`. `file_browser` mounts its `/raw` folder with `set_inline_active(!safe_mode)`. A plain `directory` gets the same behaviour for its own users.
- **Caching.** The headers of a `download_active` answer depend on the mode, so (a) the ETag carries a one-letter suffix for how the file is sent (`-p` as it is, `-s` sandboxed inline, `-a` download) and (b) the answer says `Cache-Control: no-cache`, also on its 304. Without (a) a browser that cached `index.html` as a download kept the old `Content-Disposition` through every 304 after the mode changed (the validator was the same); without (b) it did not even ask while its heuristic freshness lasted (hours for an old file), so changing the mode appeared to do nothing. Found on the owner's machine and reproduced in headless Chromium; the cost is one conditional request per file view, a 304 when nothing changed. A plain `serve_directory` keeps its caching.
- `set_safe_mode(true)` is the old behaviour: html, svg, xml, js and unknown types are only downloaded and the dialog has no preview for them.
- The page learns the mode through the config object (`"safe":true|false`); a missing key means the default, not safe.
- Python mirrors it (ADR-0017, ADR-0065): `FileBrowser.set_safe_mode` and `safe_mode=`, `Directory.set_inline_active` and `inline_active=`. The CLI gets `httpp browse --safe`. The parity test needs no change because the names match.
- Names are the owner's: "safe mode", and the CLI flag is `--safe` because the default is the opposite. The default is not as reckless as the word "unsafe" suggests: what is shown is always behind the sandbox policy, which is what makes it acceptable as a default. A project that wants the old rule everywhere calls `set_safe_mode(true)`.

## Rejected alternatives

- **A setter only for svg.** html would stay a download, and the owner asked for rendered html.
- **Showing html and svg without a policy** (just dropping the attachment header). A script in such a file would run in the origin of the browser page and could call `/raw` and `/api/list` as the viewer. Rejected: the sandbox is what makes the switch acceptable.
- **A template library (mustache) for the page.** The page needs two placeholders and one more key in a JSON object; a dependency adds nothing.
- **Rendering html in the dialog (a sandboxed `<iframe>`).** Rejected by the owner: the dialog shows html as text, and the rendered page is what "Open raw" is for. It would also have needed `frame-src` in the page's policy.
- **Also showing xml and js inline.** They have no useful rendering and the same risk; they stay downloads.

## Consequences

- The default is exactly the old behaviour; the existing tests for active types pass unchanged.
- With safe mode off, a rendered html page cannot run its own scripts, so a page that needs JavaScript shows without it. That is deliberate. A form in it cannot submit (`form-action 'none'`) and nothing it links to is fetched by the page itself (`default-src 'none'`).
- A relative stylesheet or image next to the html loads, but is subject to the same download rules as any other file in `/raw` (a `.css` is served with `attachment`, which browsers ignore for sub-resources).

## Verification

Sandbox, Linux, Release, Python 3.12, headless Chromium (from npm). C++ suite: 550 passed (new: the ETag and `Cache-Control` of a `download_active` answer follow the mode; by default html and svg are inline behind the sandbox and `?download=1` and the other active types still download; `set_safe_mode(true)` makes html and svg downloads too; `directory::set_inline_active` with and without `set_download_active`; the older "active types are downloads" tests now say `set_safe_mode(true)`). Python wheel layout built from the final tree: 615 passed, 7 skipped (after rebasing onto upstream's logging/CLI commit c326808, which merged without a textual conflict), including the new `test_file_browser.py` and `test_builders.py` cases, the parity test and the CLI test for `httpp browse` with and without `--safe`.
Checked by hand in headless Chromium against `httpp browse` (no flag): in the dialog an html file is text and an svg is an image; "Open raw" opens a rendered page (the h1 is there, the script did not run) and a rendered svg, with no download; with `--safe` the same URL is a download. A browser profile that had loaded the file in one mode and then met a server in the other mode showed the old behaviour until `Cache-Control: no-cache` and the mode-dependent ETag were added (see Caching).
Not verified: Windows, macOS, sanitizers, other browsers.
