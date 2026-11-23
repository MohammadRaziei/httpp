# ADR-0097: The library does what `httpp curl` used to do itself: elapsed time, retry, and saving to a file

- **Status:** implemented in C++ (built and tested on Linux); each part says whether Python is bound (merged ADR)
- **Merged on:** 2026-11-21, at the request of the project owner: the decisions of one subject were spread over 4 ADRs. The text of every part is kept, as a section below, in the order it was decided.

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history. Where a part and this summary disagree, this summary wins.

- **`response::elapsed_ms` and `response::bytes_received`** are filled on success and on failure: the time from the call until the last byte or the failure (redirects included, the same span `max_time()` limits) and the body bytes of the final response before decoding.
- **Retry is the library's job:** `retry(n)`, `retry_delay(seconds)` (0 means 1, 2, 4, ... seconds, 600 at most), `retry_connrefused(bool)` and `on_retry` are in the shared settings, so they work on `request`, `client`, `async_client` and `download`. A timeout, 408, 429, 500, 502, 503, 504 and (with `retry_connrefused`) a failed connection are tried again; the head of an answer that will be retried never reaches `on_head()`.
- **Saving to a file is the library's job:** `request::save_to(path)`, `save_in(folder)` (the name comes from Content-Disposition, else from the URL), `create_dirs(bool)`, `overwrite(bool)` and `include_head(bool)`. The body is written as it arrives and `response::body` stays empty; the file is opened only when the final head is accepted (not for a redirect or an answer that will be retried).
- **Retry on a download works:** a 206 is placed in the file by the start of its `Content-Range`, so a retried try does not write the same bytes twice; a 206 without a usable `Content-Range`, or one that would leave a hole, ends the download with an error.
- The parts say which of these are bound in Python and which are listed in `NOT_EXPOSED`.

## Parts
### `response::elapsed_ms` and `response::bytes_received`, so the curl command does not time its own call

- **Status:** implemented in C++ (built and tested on Linux); Python is not bound yet
- **Decided by:** the owner asked that `httpp curl` be a thin call into `httpp::request` and that logic such as the timeout report live in the library
- **Source:** `include/httpp/client.hpp` (`response`), `src/core/http_exchange.cpp` (`run_chain`), `src/core/async_http.cpp` (`exchange`), `src/cli/httpp_cli.cpp` (`curl_transfer`)
- **Follows:** ADR-0077 and 0093

#### Context

The last commit made `httpp curl` print `error: timed out after 2003 ms with 120 bytes received`, like curl does, so a user can tell a server that never answered from one that was still sending. To do that `curl_transfer` kept its own `std::chrono::steady_clock::time_point attempt_started`, set it before every attempt, and counted the body bytes in its `on_body` callback (`downloaded`).

That was logic of the request in the front end. The library already runs the clock (`max_time` is measured from the first hop) and already sees every body byte, but it did not say what it saw. Any other caller (a C++ program, the Python bindings later) that wanted the same report had to time and count by itself, and could not do it as exactly as the engine, because the engine's clock includes the queue and the redirect hops.

#### Decision

1. `httpp::response` has two client-side fields, filled on success and on failure:
   - `long long elapsed_ms`: from the call to `run()` (or `run_async()`) until the last byte or the failure, redirects included. It is the same span `max_time()` limits, so a request that hit `max_time(1)` reports a little over 1000.
   - `std::size_t bytes_received`: body bytes of the final response that arrived, before gzip or deflate decoding. With `on_body()` it is the sum of the pieces the callback was given (a piece is counted before the callback runs, so a callback that stops the transfer is included). Without it, it is what was collected into `body`. A redirect that is followed does not count, and a skipped body (`on_head` returned false, or a HEAD) is 0.
2. The clock starts in `run_chain`, once per request, and stops in a wrapper around the callback that receives the final response. The recursion over redirect hops moved into `run_hops` so the wrapper is added once, not per hop. Every path (`run()`, `run_async()`, `client`, `async_client`, `download`) goes through `run_chain`, so none needs its own code.
3. `bytes_received` is a plain counter on the engine's `exchange`, next to the code that appends or forwards the body. The engine's own retry on a dead kept-alive connection resets it with the rest of the response.
4. `httpp curl` reads the two fields. `attempt_started` and `downloaded` are gone from `curl_transfer`, and `-w %{size_download}` is `res.bytes_received`. The wording of the message stays in the CLI: it imitates curl and is output formatting, not behaviour of the request.
5. The text of `response::error_message` does not change (`"timed out"` for a step, `"total time limit reached"` for `max_time`), so nothing that matches on it breaks.

#### Rejected alternatives

- **Put the whole sentence into `error_message`** (`"timed out after 2003 ms with 120 bytes received"`). It would change a string callers may match on, and the CLI would have to avoid printing the label and the message twice. Numbers are easier to use than a sentence, and the sentence can be made from them.
- **A separate `timing` struct on `response`.** More names for two numbers, and the Python and C++ parity work (ADR-0093) would grow with it.
- **`std::chrono::milliseconds` as the type.** It would put `<chrono>` into the public header and the Python binding would have to convert it. A plain count with the unit in the name is what `timeout(long seconds)` and `max_time(long seconds)` already do.
- **Retry inside `request` (`--retry`)** was considered in the same pass and left out. It is a larger public API (`retry`, `retry_delay`, `retry_connrefused` on all three classes of ADR-0093, plus Python) and needed its own decision (done in the part "`retry`, `retry_delay`, `retry_connrefused` and `on_retry`: sending again is the library's job, not the curl command's").

#### Consequences

- `response` grows by 16 bytes. It is an exported struct: **rebuild the consumers** (same note as ADR-0093).
- The CLI has no clock and no byte counter of its own; `curl_transfer` only formats what the response says.
- Python's `Response` does not expose the two fields yet. The Cython bindings were not compiled in this change.
- `elapsed_ms` counts from the call, so it includes time spent waiting for a free connection in `async_client`; it is the time the caller waited, which is what a timeout report is about.

### `retry`, `retry_delay`, `retry_connrefused` and `on_retry`: sending again is the library's job, not the curl command's

- **Status:** implemented in C++ (built and tested on Linux); Python is not bound yet (listed in `NOT_EXPOSED`)
- **Decided by:** the owner asked that `httpp curl` be a thin call into `httpp::request` and that its logic live in the library; after the part "`response::elapsed_ms` and `response::bytes_received`, so the curl command does not time its own call" he told me to move `--retry` too
- **Source:** `include/httpp/client.hpp` (`configurable`, `settings_ref`, `request::on_retry`), `src/core/http_exchange.cpp` (`run_chain`, `run_tries`), `src/core/async_http.cpp` (`post_after`), `src/cli/httpp_cli.cpp` (`fill`)
- **Follows:** ADR-0077 and 0093 and the part "`response::elapsed_ms` and `response::bytes_received`, so the curl command does not time its own call"

#### Context

`httpp curl --retry` was a loop in `curl_transfer`: it knew which answers may pass on another try (a timeout, 408, 429, 500, 502, 503, 504, and a failed connection with `--retry-connrefused`), computed the wait (1, 2, 4, ... up to 600 seconds), slept on the calling thread, and had a line in its `on_head` so that an answer about to be sent again was not written to the output. Every other caller of the library had to write the same loop, and could not get the last part right for a streamed body: only the library knows which answer is the last one before it hands the head to `on_head()`.

#### Decision

1. Three settings in `configurable` (so on `request`, `client` and `async_client`, and a session hands them to the requests it makes), named like the curl flags:
   - `retry(int times)`: more tries after one that may pass. 0 (the default) never sends again. A negative number throws `std::invalid_argument`.
   - `retry_delay(long seconds)`: the wait before each new try. 0 (the default) is 1, 2, 4, ... seconds, 600 at most (`min(600, 2^n)`, n counting from 0). A negative number throws.
   - `retry_connrefused(bool)`: also send again after `error_kind::connection`. Off by default.
2. `request::on_retry(fn)`, with `fn(const response& last, int retry_number, long delay_seconds)`: called before each new try, before the wait begins. It is a per-request callback like `on_body`, not a setting, so it is on `request` only. It runs on the network thread; a callback that throws is ignored.
3. Which answers are worth another try is decided once, in `run_tries` (`src/core/http_exchange.cpp`): `error_kind::timeout`, the statuses 408, 429, 500, 502, 503 and 504, and `error_kind::connection` only with `retry_connrefused`. A redirect that is followed is not an answer and is not a try; a 404 never is retried. Same table as curl.
4. Every try is a whole request: it gets a copy of the spec, its own `max_time()` clock and its own redirect count, so `timeout()` and `max_time()` limit a try, not all of them together. The returned response is that of the last try, `elapsed_ms` and `bytes_received` (the part "`response::elapsed_ms` and `response::bytes_received`, so the curl command does not time its own call") included. The calls without `retry` take the old path unchanged.
5. With `on_body()` the sink of a try that can still be followed by another is wrapped: a transient status is answered with `verdict::skip`, so the body is read and thrown away and neither `on_head` nor `on_body` of the caller sees it. On the last try the head is handed on as usual. A transient answer is therefore shown to the callbacks if and only if it is the final one. A failure after a body already reached `on_body` (a timeout in the middle of it, say) is still retried and the body then arrives again from its first byte: the callbacks cannot take it back. `httpp curl` behaves the same as before.
6. The wait is a timer, not a sleep: `async_http::post_after(delay, fn)` runs `fn` on the engine's loop thread when the time has passed, so other requests go on, and `run_async` and `async_client` need no thread of their own. `run()` still blocks its caller for the wait, as it blocks for the request. The wait holds the connection slot of an `async_client`.
7. `httpp curl` copies `--retry`, `--retry-delay` and `--retry-connrefused` into those settings and, unless `-s`, prints `Warning: transient problem, will retry in N seconds (K of M)` from `on_retry`, as before. It has no loop, no clock, no transient table and no sleep of its own, and no `#include <thread>`.
8. The wait is in whole seconds (curl's unit); `retry_delay` takes no fraction. (Later, ADR-0110: `retry_delay` and the other waits are `double` seconds, so a fraction is a real wait.)

#### Rejected alternatives

- **A `retry` policy object** (`retry_policy{ .times, .delay, .on }`). More names and a new type for three plain values, and ADR-0093 already has a place for session settings. Rejected for the same reason as a `timing` struct in the part "`response::elapsed_ms` and `response::bytes_received`, so the curl command does not time its own call".
- **A caller-supplied predicate** (`retry_if(fn)`). curl has a fixed list and so does every script this command replaces; a predicate can be added later without changing these three names.
- **Retry only in `run()`** (a loop with `sleep_for`). `run_async()` and `async_client` would then ignore a setting that `configurable` gives them, and a sleeping `done` callback would freeze the shared network thread for every other request.
- **A detached thread per wait.** Works, costs a thread per waiting request; the engine already owns a loop with timers.
- **`unlimited` for `retry`** (ADR-0095). Retrying forever is not a limit being lifted but a different policy; curl's `--retry` takes a number too.
- **Python in the same change.** The bindings were not compiled (no Cython in the sandbox), so the five names are listed in `tests/python/test_cpp_python_parity.py` `NOT_EXPOSED` with this ADR, as `on_body` is, and bound next.

#### Consequences

- `session_settings`, `request_spec` and the exported `settings_ref` grow: **rebuild the consumers** (same note as ADR-0093 and the part "`response::elapsed_ms` and `response::bytes_received`, so the curl command does not time its own call").
- `httpp curl --retry` keeps its flags, its warning text and its exit codes; what changed is who sends the request again.
- A C++ program, and later Python, gets `--retry` without a loop of its own, and a streamed download never shows a transient answer by accident.
- An engine destroyed while a retry waits never fires its timer. Both engines of the process are leaked on purpose (see `async_http.hpp`), so this cannot happen today; a new short-lived engine would have to cancel.

### `request::save_to`, `save_in`, `create_dirs`, `overwrite`, `include_head`: writing the answer to a file is the library's job, not the curl command's

- **Status:** implemented in C++ (built and tested on Linux); Python is not bound yet (listed in `NOT_EXPOSED`); the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-20
- **Decided by:** the owner asked that `httpp curl` be a thin call into the library; he agreed to `save_to` with the file mechanics shared with `download`, and said curl need not be tied to `request` alone
- **Source:** `include/httpp/client.hpp` (`request::save_to` and its four options, `response::saved_as`, `error_kind::file`), `src/core/client.cpp` (`request::impl::make_spec`), `src/core/internal/file_writer.hpp`, `src/core/internal/http_exchange.{hpp,cpp}` (`request_spec::on_finish`), `src/core/download.cpp`, `src/cli/httpp_cli.cpp` (`curl_transfer`), `tests/cpp/test_client_save_to.cpp`
- **Follows:** ADR-0077 and 0093, the part "`response::elapsed_ms` and `response::bytes_received`, so the curl command does not time its own call" and the part "`retry`, `retry_delay`, `retry_connrefused` and `on_retry`: sending again is the library's job, not the curl command's"

#### Context

`httpp curl -o`, `-O`, `-J`, `--output-dir`, `--create-dirs` and `-i` were an `std::ofstream` and about sixty lines in `curl_transfer`: open the file when the final head is accepted (not for a redirect, not for a 503 that retry sends again, not under `-f`), make the folder, refuse to replace a file under `-J`, write each piece and stop the transfer when a write fails, put the head in front of the body for `-i`. A C++ program that wanted the same had to write the same lines around `on_head()` and `on_body()`. One of them was also wrong: after a retry that followed a cut-off body, `file.open()` was called on a file that was already open, which fails, so `--retry` with `-o` could not recover (found by reading the code; not reproduced on the old binary).

`httpp::download` is the other way a file is written. It is the right tool for a file's life cycle (`.part`, resume, SHA-256, a lock, progress) but it cannot stand in for `curl -o`: it writes only a good status (`accepts()` in `download.cpp`), while curl writes an error page too; it has no `include_head()` and no `on_head()`; and it is a builder of its own. ADR-0093 gave it the shared settings, not these semantics.

#### Decision

1. Five members on `request` (the `&&` forms as for the other setters):
   - `save_to(path)`: the body of the final response is written to this file as it arrives, and `response::body` stays empty (like `on_body()`).
   - `save_in(folder)`: the same, the name chosen by the answer: `response::filename()` (Content-Disposition), else the last segment of the request URL (`url_filename`), else `error_kind::file` ("no file name to use in '...'"). `folder` "" is the working folder. The last of `save_to` and `save_in` wins.
   - `create_dirs(bool)`: make the missing folders above the file first. Default false.
   - `overwrite(bool)`: default true. With false a file that is there is an error (`Refusing to overwrite 'P': File exists`) and is not touched.
   - `include_head(bool)`: write `HTTP/1.1 200` and the headers, CRLF-ended, then an empty line, before the body. Default false. `response::bytes_received` still counts the body only.
2. **When the file is opened:** in the sink's `on_head`, after the caller's `on_head()` (if set) has said yes and after the retry wrapper (the part "`retry`, `retry_delay`, `retry_connrefused` and `on_retry`: sending again is the library's job, not the curl command's") and the redirect logic have let the head through. So a followed redirect, a transient answer that is sent again, an `on_head()` that returns false (`-f`) and a request that never gets an answer leave nothing behind. The file is opened (truncated) at **every** accepted head, so a retry after a cut-off body starts it again.
3. **What is written:** every answer that reaches the sink, an error page included (curl `-o`; `download` keeps its own rule). A body cut off in the middle leaves the file as far as it got; there is no `.part`.
4. **Errors:** a new `error_kind::file` ("file") with a message that names the path: `cannot write 'P'` (open or write failed, a missing folder without `create_dirs`), `cannot create the folder of 'P'`, `Refusing to overwrite 'P': File exists`, `no file name to use in 'URL'`. The sink aborts the transfer and the final response carries the error and the status of the answer that was being written. The texts are those `httpp curl` printed before, so its stderr and its exit code 23 do not change (one wording, see below).
5. **`response::saved_as`:** the path that was opened (with `save_in`, the folder joined with the name), "" when none was. It is set also after a failure in the middle of the body.
6. **`on_body()` as well:** when set together with `save_to`/`save_in` it still gets every piece (the file first). A false from either stops the transfer.
7. **How:** the mechanics (create the folder, refuse an existing file, open, write, say what went wrong) are `detail::file_writer` in `internal/file_writer.hpp`, used by this sink and by `download`'s `file_target` (which keeps its own messages and its own rule about the status). `request_spec` gets a general `on_finish` hook that `run_chain` calls once with the final response, where the file is closed (so it is flushed before the caller reads it) and `saved_as` and the file error are set. The sink and the hook copy what they need instead of keeping `this`: the sink runs on the network thread, maybe after a temporary `request` is gone (`run_async`).
8. **`httpp curl`** passes `-o`/`-O` as `save_to(path)`, `-J` as `save_in(--output-dir).overwrite(false)`, `--create-dirs` as `create_dirs`, `-i`/`-I` as `include_head`, and maps `error_kind::file` to exit code 23 with the message of the response. It has no `ofstream` for the body, no folder code, no overwrite check and no name logic of its own; only standard output is still written by `put`. `-D` (the header file) is a different file and stays as it was.

#### Taken by default while implementing (owner to confirm)

- **`overwrite(false)` with `save_in` also refuses the URL-name fallback.** Before, `-J` refused only a name that came from the answer and replaced a file named after the URL when the answer had no name. Refusing both is the safe side and keeps the option's meaning simple.
- **`-I` with a file:** `curl_transfer` no longer returns false from `on_head` for a `-I` that writes to a file (the file is made, with the head in it). A HEAD has no body, so nothing changes for it; `-I -X GET -o file` now writes the body after the head, where it used to discard it. Standard output keeps the old behavior.
- **The message of a missing name** is `no file name to use in 'URL'` (it said `for -O`, a flag the library does not know).
- **`error_kind::file` sits before `other`**, so `other` changes its number: rebuild consumers (as in ADR-0093, the part "`response::elapsed_ms` and `response::bytes_received`, so the curl command does not time its own call" and the part "`retry`, `retry_delay`, `retry_connrefused` and `on_retry`: sending again is the library's job, not the curl command's").

#### Rejected alternatives

- **`download` as the engine of `curl -o`:** needs headers, a method, retry, `on_head()` and a way to write an error page and a head; it would grow into a second `request`.
- **Everything in `request::download_to(path)` with `.part`, resume and a lock** (rejected already by ADR-0093): puts the file life cycle into every request.
- **Leave the file in the CLI and share only a helper:** the callers of the library still would not get the open-at-the-final-head rule or the retry fix.
- **A `file` error as `error_kind::other` with a message:** a caller could not tell a full disk from a bad certificate without parsing text, and the curl command needs the difference for its exit code.

#### Consequences

- A C++ program writes the answer of a request to a file in one call, with the rules curl has, and `--retry` with `-o` works after a cut-off body.
- `response` and `error_kind` change: **rebuild the consumers.** Python's `Response` does not show `saved_as` and `Request` has no `save_to` yet; they are in `NOT_EXPOSED` with this ADR.
- `download.hpp` no longer calls itself the one way to write a file; it says what it is for and points here for the other case.
- Next steps left open: the proxy chosen for every hop of a redirect (ADR-0067 says a request never reads the environment on its own, so it would be an opt-in setting), a request body streamed from a file (`-T`, `-d @file`), a trace hook for `-v`, and `-D` as a library option.

### `retry` on a download: a 206 is placed in the file by its Content-Range

- **Status:** implemented in C++ (built and tested on Linux); Python is not bound yet (`NOT_EXPOSED`); the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-20
- **Decided by:** the project owner ("maybe something could be written to help you", then "start implementing"); the details were left to the implementer
- **Source:** `include/httpp/download.hpp`, `src/core/download.cpp` (`file_target::on_head`, `content_range_start`), `src/core/internal/file_writer.hpp` (`open_at`), `tests/cpp/test_download_retry.cpp`
- **Follows:** ADR-0051 and 0093, the part "`retry`, `retry_delay`, `retry_connrefused` and `on_retry`: sending again is the library's job, not the curl command's" and the part "`request::save_to`, `save_in`, `create_dirs`, `overwrite`, `include_head`: writing the answer to a file is the library's job, not the curl command's"

#### Context

ADR-0093 deleted `retry`, `retry_delay` and `retry_connrefused` on `download` with the reason "a try that fails in the middle of the body would write the file twice; it needs resume to be safe". Reading `download.cpp` shows that is true for one case only.

Without `resume()` a try always asks for the whole file (`Range` is not sent), and `file_target::on_head` opens the file with truncation, so a retry already starts the file again. With `resume()` the request carries `Range: bytes=<size of the .part>-` and `transfer()` builds it once; the part "`retry`, `retry_delay`, `retry_connrefused` and `on_retry`: sending again is the library's job, not the curl command's"'s `run_tries` copies that spec for every try. A try that appends 300 bytes to the `.part` and is cut off is followed by a try that sends the **same** `Range`, and the old sink (`offset_ > 0` means append) wrote those 300 bytes a second time. That is the real hazard, and it is a property of where the sink writes, not of retry.

The first idea was a hook that rewrites the `Range` before each try from the size of the `.part`. The owner's remark that the file could carry its own help led to a simpler rule that needs no hook and no extra bytes in the file.

#### Decision

1. **The sink puts the bytes where the server says they start.** For a 206, `file_target` reads the first byte from `Content-Range` (`bytes 4-9/10` is 4) and writes from there. For any other accepted status (a 200: the server ignored the Range or none was sent) it starts the file again from byte 0, as before. What was asked for (`offset_` at construction) no longer decides where to write.
2. **Writing at an offset needs no truncate:** `detail::file_writer::open_at(file, offset)` opens the file without cutting it, seeks to `offset` and writes over what was after it. httpp uses no `std::filesystem` (ADR-0054) and `httpp::path` has no truncate, so the file is rewritten in place. A retried try writes the same bytes of the same resource at the same places, so the result is the same as after a truncate.
3. **No guessing:** a 206 without a usable `Content-Range`, and a `Content-Range` that starts after the end of the file (it would leave a hole), end the download with an error that says so. The `.part` is then dealt with by the rules that already exist.
4. **`retry`, `retry_delay` and `retry_connrefused` are not deleted on `download` any more.** They are the ones of `configurable` (the part "`retry`, `retry_delay`, `retry_connrefused` and `on_retry`: sending again is the library's job, not the curl command's"), each try with its own `timeout()` and `max_time()`. `auto_compression` stays deleted (ADR-0093). The Range is not recomputed for a retry: with `resume()` a retry takes again the bytes the failed try had already written, which is slower than needed and correct.
5. **A transient status** (503 and the others of the part "`retry`, `retry_delay`, `retry_connrefused` and `on_retry`: sending again is the library's job, not the curl command's") is answered by the retry wrapper before `file_target` sees it, so its page is never written; when the retries are used up the last answer reaches `file_target`, is not a good status and ends the download as before.

#### Taken by default while implementing (owner to confirm)

- **A longer old tail stays.** If the resource gets shorter between two tries, the bytes the first try wrote past the new end remain after it, because nothing can cut the file. The checksum (`sha256`) would catch it; a `path::truncate` would remove it. It needs a Windows implementation that I could not build, so it is left out of this step.
- **`Content-Range` is trusted over the request.** A server that answers a different start than the one asked for is placed where it says; before, the sink appended at the asked-for offset.
- **No progress is kept across tries.** A cut-off try's bytes are fetched again. Skipping them needs a hook that rewrites the Range before a try; it is an optimisation and can be added without changing the rule above.
- **`If-Range` / ETag is still not sent.** A file that changes on the server between two resumes is the same risk as before. A small file next to the `.part` that remembers the ETag is the way to close it, and is a separate step.
- **A try that wrote every byte and was cut off at the very end** is followed by a Range that starts at the end of the file; most servers answer 416, which is not retried, so that download fails while its `.part` is complete. The next run with `resume()` renames it (existing code). Closing the gap needs the same hook as above.

#### Rejected alternatives

- **Write a marker or a footer inside the `.part`:** the file is renamed to the final name, so anything inside it must be cut off before that, and a cut between the two steps would be a new way to corrupt it.
- **Make `download` run its own retry loop** with a fresh Range each time: copies the table of transient answers and the back-off that the part "`retry`, `retry_delay`, `retry_connrefused` and `on_retry`: sending again is the library's job, not the curl command's" put in one place.
- **A `before_try` hook now:** useful later (above), not needed to make retry correct.
- **`path::truncate`:** see the first point under "Taken by default".

#### Consequences

- `httpp::download(url, file).retry(3).run()` works with and without `resume()`, and a cut-off body is never written twice.
- `download` changes behavior in one place for everyone: a 206 is positioned by `Content-Range`, and one without it is an error instead of being appended.
- `tests/python/test_download_settings.py` no longer asserts that `retry` is missing; Python's `Download` does not expose the three names yet (parity `NOT_EXPOSED`, this part).
- Not done: Python binding, the `httpp download --retry` flag, Windows and macOS.
