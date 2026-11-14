# ADR-0070: The file browser has a safe mode; without it html and svg are shown inside a sandbox

- **Status:** accepted; amends ADR-0032 (html and svg were always downloads) and ADR-0065
- **Decided by:** the project owner ("a method on the file browser class that controls it; the safe mode keeps what we have; html as text in the dialog, but really rendered when \"Open raw\" opens it, no iframe"); the details are Claude's
- **Source:** `include/httpp/file_browser.hpp`, `include/httpp/server.hpp`, `src/core/file_browser.cpp`, `src/core/server.cpp`, `src/assets/file_browser.html`, `src/cli/httpp_cli.cpp`, `src/bindings/python/cy/{file_browser,directory,support}.pxi`

## Context

ADR-0032 made every type that can run code in the page's origin (html, svg, xml, js, unknown) a download, and the dialog has no preview for them. That is the right default for a folder nobody vouches for, but it is also why an svg showed "No preview for this file type" and why "Open raw" saved the file instead of opening it. Someone who browses their own folder wants to see both.

## Decision

- `file_browser::set_safe_mode(bool on)`, default `true`: nothing changes. With `false`:
  - **Dialog:** an svg is shown as an image (`<img>`: scripts and outside requests never run there). An html file stays in the text preview it always had; there is no frame, so the page's own policy is unchanged.
  - **"Open raw":** html and svg are sent inline, not as an attachment, with `Content-Security-Policy: sandbox; default-src 'none'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; font-src 'self' data:; media-src 'self'; base-uri 'none'; form-action 'none'`. Without `allow-scripts` the sandbox stops every script, and `default-src 'none'` stops every request to another origin, so even a hostile file cannot reach the page, the cookies or the network. Images, styles, fonts and media of the same server still load, so a plain page renders as written.
  - **Still downloads:** xml, js, every unknown type, and any file asked for with `?download=1`.
  - The server side is a separate switch, `directory::set_inline_active(bool)`, default `false`, which only has an effect together with `set_download_active(true)`. `file_browser` mounts its `/raw` folder with `set_inline_active(!safe_mode)`. A plain `directory` gets the same behaviour for its own users.
- The page learns the mode through the config object (`"safe":true|false`); a missing key means safe.
- Python mirrors it (ADR-0017, ADR-0065): `FileBrowser.set_safe_mode` and `safe_mode=`, `Directory.set_inline_active` and `inline_active=`. The CLI gets `httpp browse --unsafe`. The parity test needs no change because the names match.
- Name: "safe mode" is the owner's; the CLI flag is `--unsafe` because a flag that turns a default off reads better as the thing it enables.

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

Sandbox, Linux, Release, Python 3.12. C++ suite: 548 passed (new: safe mode is the default and html and svg are downloads; without safe mode html and svg are inline behind the sandbox, `?download=1` and the other active types still download; `directory::set_inline_active` with and without `set_download_active`). Python wheel layout built from the final tree: 613 passed, 7 skipped (after rebasing onto upstream's logging/CLI commit c326808, which merged without a textual conflict), including the new `test_file_browser.py` and `test_builders.py` cases, the parity test and the CLI test for `httpp browse --unsafe`. `httpp browse --unsafe` checked by hand with curl: html and svg inline with the sandbox policy, xml an attachment, the page config carries `"safe":false`.
Not verified: the dialog in a real browser (the svg `<img>` path is written against the browser rules, not run; no browser was available), Windows, macOS, sanitizers.
