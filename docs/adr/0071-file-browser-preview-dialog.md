# ADR-0071: The preview dialog has icon buttons and closes with an animation

- **Status:** accepted; amends ADR-0032 and ADR-0070 (the preview dialog)
- **Decided by:** the project owner (asked for icons with tooltips in the top right, a slightly larger dialog, a better design, and a smooth close when clicking outside); the details are Claude's
- **Source:** `src/assets/file_browser.html`, `tests/cpp/test_file_browser.cpp`

## Context

The dialog showed three text buttons (Download, Open raw, Close) in a row under the preview, and the title carried the size in parentheses. It was narrow for video, closed with no transition and could only be closed with its button or the Escape key.

## Decision

- **Three icon buttons at the top right**: download, open raw (opens the file in a new tab) and close. Each has an `aria-label` and a tooltip drawn in CSS from `data-tip` (it appears below the icon after a short delay, also on keyboard focus). They use the page's variables, so light, dark and the accent colour apply. Inline SVG, as the other icons.
- **Header**: the file name is the title and the size is a quiet second line.
- **Larger dialog**: width `min(60rem, 94vw)` (was 52rem), preview height up to 66vh (was 62vh), a larger radius, a shadow and a blurred backdrop.
- **Opening and closing animate** (about 0.2 s in, 0.16 s out: fade, a small move and scale; the backdrop fades). Closing adds a class and calls `close()` after the animation time, so the `close` event still stops the media and clears the preview. The Close button, the Escape key (the `cancel` event is intercepted) and a click outside the dialog all go through the same function. A click closes only when the press and the release are both outside the dialog box, so dragging a slider and letting go outside does not close it. With `prefers-reduced-motion` there is no animation and the dialog closes at once.

## Rejected alternatives

- Native `title` tooltips: slow, unstyled, and not shown on touch or keyboard focus.
- A setting to turn click-outside off: nothing asked for it, and the Close button and Escape stay available either way.
- Closing on `animationend`: it needs one more event path (the backdrop animation reports too); a fixed timeout that matches the CSS time is simpler.

## Consequences

- The page is about 25 KB before compression (it is embedded compressed, ADR-0031).
- Verified: the script runs in jsdom (icons and tooltips present for the three buttons with the right links, the old text buttons gone, close by button, by outside click and by Escape each go through the closing class and then close, a click without an outside press stays open, the media player is cleared on close, an image preview still opens, no script error). A new C++ test (`the_preview_dialog_has_icon_buttons_with_tooltips`) is written to check the shipped page, but it was not compiled or run here.
- **Not verified:** a real browser. The look of the dialog and tooltips, the animation timing, the blur on the backdrop, the `cancel` interception on Escape (including pressing Escape twice quickly, which some browsers treat as a forced close), tooltip placement near the right edge on a narrow screen, and touch devices are untested.
