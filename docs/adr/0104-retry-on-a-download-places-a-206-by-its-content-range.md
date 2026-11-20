# ADR-0104: `retry` on a download: a 206 is placed in the file by its Content-Range

- **Status:** implemented in C++ (built and tested on Linux); Python is not bound yet (`NOT_EXPOSED`); the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-20
- **Decided by:** the project owner ("maybe something could be written to help you", then "start implementing"); the details were left to the implementer
- **Source:** `include/httpp/download.hpp`, `src/core/download.cpp` (`file_target::on_head`, `content_range_start`), `src/core/internal/file_writer.hpp` (`open_at`), `tests/cpp/test_download_retry.cpp`
- **Follows:** ADR-0052, ADR-0099, ADR-0101, ADR-0102

## Context

ADR-0101 deleted `retry`, `retry_delay` and `retry_connrefused` on `download` with the reason "a try that fails in the middle of the body would write the file twice; it needs resume to be safe". Reading `download.cpp` shows that is true for one case only.

Without `resume()` a try always asks for the whole file (`Range` is not sent), and `file_target::on_head` opens the file with truncation, so a retry already starts the file again. With `resume()` the request carries `Range: bytes=<size of the .part>-` and `transfer()` builds it once; ADR-0099's `run_tries` copies that spec for every try. A try that appends 300 bytes to the `.part` and is cut off is followed by a try that sends the **same** `Range`, and the old sink (`offset_ > 0` means append) wrote those 300 bytes a second time. That is the real hazard, and it is a property of where the sink writes, not of retry.

The first idea was a hook that rewrites the `Range` before each try from the size of the `.part`. The owner's remark that the file could carry its own help led to a simpler rule that needs no hook and no extra bytes in the file.

## Decision

1. **The sink puts the bytes where the server says they start.** For a 206, `file_target` reads the first byte from `Content-Range` (`bytes 4-9/10` is 4) and writes from there. For any other accepted status (a 200: the server ignored the Range or none was sent) it starts the file again from byte 0, as before. What was asked for (`offset_` at construction) no longer decides where to write.
2. **Writing at an offset needs no truncate:** `detail::file_writer::open_at(file, offset)` opens the file without cutting it, seeks to `offset` and writes over what was after it. httpp uses no `std::filesystem` (ADR-0054, 0058) and `httpp::path` has no truncate, so the file is rewritten in place. A retried try writes the same bytes of the same resource at the same places, so the result is the same as after a truncate.
3. **No guessing:** a 206 without a usable `Content-Range`, and a `Content-Range` that starts after the end of the file (it would leave a hole), end the download with an error that says so. The `.part` is then dealt with by the rules that already exist.
4. **`retry`, `retry_delay` and `retry_connrefused` are not deleted on `download` any more.** They are the ones of `configurable` (ADR-0099), each try with its own `timeout()` and `max_time()`. `auto_compression` stays deleted (ADR-0101). The Range is not recomputed for a retry: with `resume()` a retry takes again the bytes the failed try had already written, which is slower than needed and correct.
5. **A transient status** (503 and the others of ADR-0099) is answered by the retry wrapper before `file_target` sees it, so its page is never written; when the retries are used up the last answer reaches `file_target`, is not a good status and ends the download as before.

## Taken by default while implementing (owner to confirm)

- **A longer old tail stays.** If the resource gets shorter between two tries, the bytes the first try wrote past the new end remain after it, because nothing can cut the file. The checksum (`sha256`) would catch it; a `path::truncate` would remove it. It needs a Windows implementation that I could not build, so it is left out of this step.
- **`Content-Range` is trusted over the request.** A server that answers a different start than the one asked for is placed where it says; before, the sink appended at the asked-for offset.
- **No progress is kept across tries.** A cut-off try's bytes are fetched again. Skipping them needs a hook that rewrites the Range before a try; it is an optimisation and can be added without changing the rule above.
- **`If-Range` / ETag is still not sent.** A file that changes on the server between two resumes is the same risk as before. A small file next to the `.part` that remembers the ETag is the way to close it, and is a separate step.
- **A try that wrote every byte and was cut off at the very end** is followed by a Range that starts at the end of the file; most servers answer 416, which is not retried, so that download fails while its `.part` is complete. The next run with `resume()` renames it (existing code). Closing the gap needs the same hook as above.

## Rejected alternatives

- **Write a marker or a footer inside the `.part`:** the file is renamed to the final name, so anything inside it must be cut off before that, and a cut between the two steps would be a new way to corrupt it.
- **Make `download` run its own retry loop** with a fresh Range each time: copies the table of transient answers and the back-off that ADR-0099 put in one place.
- **A `before_try` hook now:** useful later (above), not needed to make retry correct.
- **`path::truncate`:** see the first point under "Taken by default".

## Consequences

- `httpp::download(url, file).retry(3).run()` works with and without `resume()`, and a cut-off body is never written twice.
- `download` changes behavior in one place for everyone: a 206 is positioned by `Content-Range`, and one without it is an error instead of being appended.
- `tests/python/test_download_settings.py` no longer asserts that `retry` is missing; Python's `Download` does not expose the three names yet (parity `NOT_EXPOSED`, ADR-0104).
- Not done: Python binding, the `httpp download --retry` flag, Windows and macOS.
