# ADR-0070: The file browser plays audio and video with its own controls

- **Status:** accepted; amends ADR-0032 (the preview part of the page)
- **Decided by:** the project owner (asked for a theme for audio and video that matches the page); the details are Claude's
- **Source:** `src/assets/file_browser.html`, `tests/cpp/test_file_browser.cpp`

## Context

The preview dialog used `<audio controls>` and `<video controls>`. The browser draws those controls itself, so they ignored the page's light and dark theme and the accent colour, looked different in every browser, and could not skip to the next file of a folder.

## Decision

- The media element stays, without `controls`, and the page draws the bar. It is built from the same CSS variables as the rest of the page (`--bg`, `--fg`, `--row`, `--border`, `--accent`), so light, dark, system and a custom accent follow with no extra code. Still one HTML file with inline CSS, JS and SVG, no dependency, no new request, so the Content-Security-Policy of ADR-0032 is unchanged and so is the C++ and Python API.
- Controls: play and pause, a seek bar with the buffered range (the position is committed when the pointer is released, so a drag does not send a range request per pixel), time, mute and volume (the volume is kept in `localStorage`), speed (a button that cycles 1, 1.25, 1.5, 2, 0.75), full screen for video. Keys inside the player: space, left and right (5 s), up and down (volume), `m`, `f`. Video: a click toggles play, a double click toggles full screen, the bar hides after 2.5 s without movement. Volume and speed are hidden on narrow screens.
- **Previous and next** appear when the folder has more than one file of that kind (audio or video), in the order the list shows, with the filter applied. A file that ends starts the next one. Previous restarts the current file when it has played more than 3 s.
- Closing the dialog stops the element and removes its `src`, so a large file stops downloading.
- A webm file without a duration reports `Infinity`; the player seeks far past the end once so the browser finds the duration.
- A file the browser cannot decode shows a message that points to Download and Open raw.

## Rejected alternatives

- A media library (Plyr, Video.js): a dependency of tens of KB against a page of about 13 KB, and ADR-0032 asked for a page that can be audited in one read.
- Styling the native controls: only a few pseudo-elements are exposed and they differ between engines.
- A playlist panel and thumbnails: more UI than the need; previous and next cover the case of a folder of songs.

## Consequences

- The page grows from about 13 KB to about 22 KB before compression (it is embedded compressed, ADR-0031).
- Verified: the script runs in jsdom with the media API stubbed (player built for audio and for video, no `controls` attribute, play and pause, mute key, next, speed, close clears the player, no script error); a new C++ test (`the_page_ships_its_own_media_player_and_no_native_controls`) is written to check that the shipped page has the player and no native controls, but it was not compiled or run here.
- **Not verified:** a real browser. Nothing here was opened in Chromium, Firefox or Safari, so full screen, pointer dragging on the sliders, the idle bar, the duration fix and keyboard focus inside the dialog are untested. Playing a real file, a 4 GB file and a phone screen are untested too.
- Names that mix right-to-left and left-to-right text are still shown in the order the Unicode algorithm gives them in the list and in the title; this ADR does not change that.
