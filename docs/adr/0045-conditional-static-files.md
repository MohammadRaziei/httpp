# ADR-0045: Files from a mounted directory carry ETag and Last-Modified and answer conditional requests

- **Status:** accepted
- **Source:** `answer_file` and the validator helpers in `src/core/server.cpp`, the 304 rule in `src/core/async_server.cpp`, `tests/cpp/test_server_conditional.cpp`,
  `tests/python/test_httpp_basic.py`

## Context

`serve_directory` (and the raw route of the file browser, which is built on it) sent every file in full on every request, with no validators. A browser that already had the
file downloaded it again, and a resuming download could not tell that the file had been replaced since the first part was fetched. The engine also wrote
`Content-Length: 0` on a 304, which is wrong for a status that has no body.

## Decision

1. Every 200 and 206 answer for a file gets `ETag` and `Last-Modified`. The tag is strong and is made of the size, the modification time in seconds and its sub-second part
   (where the platform has one) in hex, for example `"b-6700a1c2-1d4f"`. Reading it costs one `stat`.
2. The preconditions are evaluated in the order of RFC 9110 section 13.2.2, for GET and HEAD:
   `If-Match` (strong comparison) or else `If-Unmodified-Since` give 412 when they fail; `If-None-Match` (weak comparison, a list, `*`) or else `If-Modified-Since` give 304 when
   they match; `If-Range` (a strong tag or an exact date) decides whether `Range` is honoured, and when it does not match the whole file is sent with 200.
3. A 304 carries `ETag` and `Last-Modified` and no body. The engine no longer writes `Content-Length` for a 304 (it already did not for 204 and 1xx).
4. Only the IMF-fixdate is parsed (`Sun, 06 Nov 1994 08:49:37 GMT`). A header in one of the two obsolete date formats, or one that is not a date, is ignored as if it was not
   sent, as the RFC allows.
5. Nothing is configurable and no API is added: the C++ and Python servers behave the same because both use the same core.

## Rejected alternatives

- A weak tag (`W/`) from size and seconds: cheaper to reason about, but a strong tag is required for `If-Range` and `If-Match`, which resuming downloads use.
- A hash of the content: it needs a read of the whole file per request or a cache that has to be invalidated; the size and time change whenever the file does.
- `Cache-Control` or `Expires` defaults: that is a policy for the application. Without them a browser still revalidates with the validators, which is the safe default.
- Applying the same logic to handler responses (`res.file` set by a route): the application knows its own validators, and a file served by a route may be generated.

## Consequences

- A repeat visit to a static site costs one `stat` and about 200 bytes instead of the file.
- Resuming a download across a replaced file now fails loudly (412 with `If-Match`, or a full 200 with `If-Range`) instead of splicing two different files together.
- `ponytail:` a file replaced within the same nanosecond (or within the same second on a file system or platform without sub-second times) with the same size keeps its tag.
  Upgrade path: add the inode and the change time to the tag.
- `ponytail:` the tag list parser splits at commas, so a tag that contains a comma is not recognised; the tags produced here never do.
- Verified (Linux): 9 C++ tests (validators on 200 and HEAD, 304 for a tag, a list, a weak tag, `*`, dates in both directions, the order between the two conditions, the tag
  changing with the size and the time, `If-Range` with a tag, a weak tag and a date, 412 for `If-Match` and `If-Unmodified-Since`, a missing file still 404, the file browser
  raw route), 1 Python test, and the whole existing suites.
- Not verified: Windows and macOS (the `stat` calls differ there), a file system with a different time resolution, a browser.
