# ADR-0029: Text messages are checked for UTF-8 as the bytes arrive

## Status
Accepted (2026-10-31). Amends [ADR-0026](0026-websocket.md), which checked a text message once, when it was complete.

## Context
The first Autobahn run (602 results, no FAILED) reported NON-STRICT for cases 6.4.1 to 6.4.4: a text message whose bad byte arrives in the
second fragment (or in the middle of one frame sent in chops) was refused only after the last fragment. RFC 6455 section 8.1 says to fail
the connection as soon as the invalid data is known. Waiting also meant buffering up to `max_message` bytes of data that could never be valid.

## Decision
`ws_utf8_scan(text, complete)` accepts a prefix that can still become valid UTF-8 (a character cut off at the end is fine) and reports how many
leading bytes are whole characters. The session keeps `utf8_done_`, the checked part of the message, and scans only the new bytes after every
chunk of payload, across fragments and across network chops. A bad byte closes with 1007 at once. At the end of a message everything must be
checked (`utf8_done_ == size`), otherwise a cut-off character closes with 1007. `ws_utf8_valid` (close reasons) is the scan plus "complete".

## Rejected alternatives
- A byte-at-a-time state machine (a DFA): faster in theory, more code; the scan only revisits at most 3 bytes per chunk. Upgrade path if a
  benchmark shows UTF-8 checking in the profile.
- Leaving it: the behavior was legal (NON-STRICT) but kept buffering invalid data.

## Consequences
- The checking cost is the same, spread over the chunks; nothing is rescanned except a cut-off tail of at most 3 bytes.
- Tests: `utf8_scan_...`, `bad_utf8_in_a_fragment_fails_before_...`, `..._in_chops`, `a_character_split_across_...`, `a_text_message_that_ends_inside_a_character_...`.
- **Not verified:** that Autobahn now reports OK for 6.4.x (needs the next CI run); speed (not benchmarked).
- The other NON-STRICT results (3.2, 3.3, 4.1.3, 4.1.4, 4.2.3, 4.2.4, 5.15) are protocol failures where the server closes without first
  echoing an earlier valid message. That is allowed by the RFC and was left as it is.

## Related
ADR-0026, ADR-0027.
