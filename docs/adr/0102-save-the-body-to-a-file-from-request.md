# ADR-0102: `request::save_to`, `save_in`, `create_dirs`, `overwrite`, `include_head`: writing the answer to a file is the library's job, not the curl command's

- **Status:** implemented in C++ (built and tested on Linux); Python is not bound yet (listed in `NOT_EXPOSED`); the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-20
- **Decided by:** the owner asked that `httpp curl` be a thin call into the library; he agreed to `save_to` with the file mechanics shared with `download`, and said curl need not be tied to `request` alone
- **Source:** `include/httpp/client.hpp` (`request::save_to` and its four options, `response::saved_as`, `error_kind::file`), `src/core/client.cpp` (`request::impl::make_spec`), `src/core/internal/file_writer.hpp`, `src/core/internal/http_exchange.{hpp,cpp}` (`request_spec::on_finish`), `src/core/download.cpp`, `src/cli/httpp_cli.cpp` (`curl_transfer`), `tests/cpp/test_client_save_to.cpp`
- **Follows:** ADR-0084, ADR-0097, ADR-0099, ADR-0101

## Context

`httpp curl -o`, `-O`, `-J`, `--output-dir`, `--create-dirs` and `-i` were an `std::ofstream` and about sixty lines in `curl_transfer`: open the file when the final head is accepted (not for a redirect, not for a 503 that retry sends again, not under `-f`), make the folder, refuse to replace a file under `-J`, write each piece and stop the transfer when a write fails, put the head in front of the body for `-i`. A C++ program that wanted the same had to write the same lines around `on_head()` and `on_body()`. One of them was also wrong: after a retry that followed a cut-off body, `file.open()` was called on a file that was already open, which fails, so `--retry` with `-o` could not recover (found by reading the code; not reproduced on the old binary).

`httpp::download` is the other way a file is written. It is the right tool for a file's life cycle (`.part`, resume, SHA-256, a lock, progress) but it cannot stand in for `curl -o`: it writes only a good status (`accepts()` in `download.cpp`), while curl writes an error page too; it has no `include_head()` and no `on_head()`; and it is a builder of its own. ADR-0101 gave it the shared settings, not these semantics.

## Decision

1. Five members on `request` (the `&&` forms as for the other setters):
   - `save_to(path)`: the body of the final response is written to this file as it arrives, and `response::body` stays empty (like `on_body()`).
   - `save_in(folder)`: the same, the name chosen by the answer: `response::filename()` (Content-Disposition), else the last segment of the request URL (`url_filename`), else `error_kind::file` ("no file name to use in '...'"). `folder` "" is the working folder. The last of `save_to` and `save_in` wins.
   - `create_dirs(bool)`: make the missing folders above the file first. Default false.
   - `overwrite(bool)`: default true. With false a file that is there is an error (`Refusing to overwrite 'P': File exists`) and is not touched.
   - `include_head(bool)`: write `HTTP/1.1 200` and the headers, CRLF-ended, then an empty line, before the body. Default false. `response::bytes_received` still counts the body only.
2. **When the file is opened:** in the sink's `on_head`, after the caller's `on_head()` (if set) has said yes and after the retry wrapper (ADR-0099) and the redirect logic have let the head through. So a followed redirect, a transient answer that is sent again, an `on_head()` that returns false (`-f`) and a request that never gets an answer leave nothing behind. The file is opened (truncated) at **every** accepted head, so a retry after a cut-off body starts it again.
3. **What is written:** every answer that reaches the sink, an error page included (curl `-o`; `download` keeps its own rule). A body cut off in the middle leaves the file as far as it got; there is no `.part`.
4. **Errors:** a new `error_kind::file` ("file") with a message that names the path: `cannot write 'P'` (open or write failed, a missing folder without `create_dirs`), `cannot create the folder of 'P'`, `Refusing to overwrite 'P': File exists`, `no file name to use in 'URL'`. The sink aborts the transfer and the final response carries the error and the status of the answer that was being written. The texts are those `httpp curl` printed before, so its stderr and its exit code 23 do not change (one wording, see below).
5. **`response::saved_as`:** the path that was opened (with `save_in`, the folder joined with the name), "" when none was. It is set also after a failure in the middle of the body.
6. **`on_body()` as well:** when set together with `save_to`/`save_in` it still gets every piece (the file first). A false from either stops the transfer.
7. **How:** the mechanics (create the folder, refuse an existing file, open, write, say what went wrong) are `detail::file_writer` in `internal/file_writer.hpp`, used by this sink and by `download`'s `file_target` (which keeps its own messages and its own rule about the status). `request_spec` gets a general `on_finish` hook that `run_chain` calls once with the final response, where the file is closed (so it is flushed before the caller reads it) and `saved_as` and the file error are set. The sink and the hook copy what they need instead of keeping `this`: the sink runs on the network thread, maybe after a temporary `request` is gone (`run_async`).
8. **`httpp curl`** passes `-o`/`-O` as `save_to(path)`, `-J` as `save_in(--output-dir).overwrite(false)`, `--create-dirs` as `create_dirs`, `-i`/`-I` as `include_head`, and maps `error_kind::file` to exit code 23 with the message of the response. It has no `ofstream` for the body, no folder code, no overwrite check and no name logic of its own; only standard output is still written by `put`. `-D` (the header file) is a different file and stays as it was.

## Taken by default while implementing (owner to confirm)

- **`overwrite(false)` with `save_in` also refuses the URL-name fallback.** Before, `-J` refused only a name that came from the answer and replaced a file named after the URL when the answer had no name. Refusing both is the safe side and keeps the option's meaning simple.
- **`-I` with a file:** `curl_transfer` no longer returns false from `on_head` for a `-I` that writes to a file (the file is made, with the head in it). A HEAD has no body, so nothing changes for it; `-I -X GET -o file` now writes the body after the head, where it used to discard it. Standard output keeps the old behavior.
- **The message of a missing name** is `no file name to use in 'URL'` (it said `for -O`, a flag the library does not know).
- **`error_kind::file` sits before `other`**, so `other` changes its number: rebuild consumers (as in ADR-0094, 0097 and 0099).

## Rejected alternatives

- **`download` as the engine of `curl -o`:** needs headers, a method, retry, `on_head()` and a way to write an error page and a head; it would grow into a second `request`.
- **Everything in `request::download_to(path)` with `.part`, resume and a lock** (rejected already by ADR-0101): puts the file life cycle into every request.
- **Leave the file in the CLI and share only a helper:** the callers of the library still would not get the open-at-the-final-head rule or the retry fix.
- **A `file` error as `error_kind::other` with a message:** a caller could not tell a full disk from a bad certificate without parsing text, and the curl command needs the difference for its exit code.

## Consequences

- A C++ program writes the answer of a request to a file in one call, with the rules curl has, and `--retry` with `-o` works after a cut-off body.
- `response` and `error_kind` change: **rebuild the consumers.** Python's `Response` does not show `saved_as` and `Request` has no `save_to` yet; they are in `NOT_EXPOSED` with this ADR.
- `download.hpp` no longer calls itself the one way to write a file; it says what it is for and points here for the other case.
- Next steps left open: the proxy chosen for every hop of a redirect (ADR-0067 says a request never reads the environment on its own, so it would be an opt-in setting), a request body streamed from a file (`-T`, `-d @file`), a trace hook for `-v`, and `-D` as a library option.
