# ADR-0113: Client uploads: a streamed request body, multipart forms, and no `upload` class

- **Status:** implemented in C++ and Python and built on Linux: the 11 tests of `tests/cpp/test_client_upload.cpp`, the clone test, the other client, redirect and retry tests, the 14 tests of `tests/python/test_client_upload.py` (Python and CLI), the CLI `curl` tests, and the Python and C++ examples ran and passed. Not run: Windows, macOS, the Docusaurus build, the full C++ suite (`async_socks5` hangs in the sandbox).
- **Date:** 2026-11-26
- **Decided by:** the project owner, after he asked whether an upload command should exist next to `download`. He approved the API sketched in the conversation (no new class; the calls go on `request`) and asked for the Python binding and the command line in the same step.
- **Source:** `include/httpp/client.hpp` (`request::field`, `file`, `file_data`, `body_file`, `progress`), `src/core/body_source.cpp` and `src/core/internal/body_source.hpp` (multipart encoder, file reader), `src/core/internal/async_http.{hpp,cpp}` (`body_reader`, `body_source`, `async_job::source`, `exchange::send_source`), `src/core/internal/http_exchange.{hpp,cpp}` (`request_spec::source`, `make_job`, `engine_for`, the 303 rule), `src/core/client.cpp`, `src/bindings/python/cy/{transfer,declarations}.pxi`, `src/cli/httpp_cli.cpp` (`-F`, `--form-string`, `-T`, `-#`), `tests/cpp/test_client_upload.cpp`, `tests/python/test_client_upload.py`, `examples/upload`
- **Follows:** ADR-0077 (streamed responses), ADR-0093 (`clone`), ADR-0097 (`error_kind::file`)

## Context

The client could send only a body that was a `std::string` in memory. `httpp curl -T file` read the whole file first, so a 4 GB upload needed 4 GB of memory, and there was no way to send a form with a file (`curl -F`). The server side already decodes `multipart/form-data` (`request::form()`, `upload_file`).

## Decision

1. **No `upload` class.** A download is a job of its own (checksum, resume, lock file, a file on disk). An upload is a request with a different body, so it is built on `request`:
   - `field(name, value)`, `file(name, path, filename = {}, content_type = {})` and `file_data(name, bytes, filename, content_type = {})` build a `multipart/form-data` body, in the order called. The method becomes POST unless `method()` was called.
   - `body_file(path)` is `curl -T`: the whole body is the file. The method becomes PUT unless set; the Content-Type is guessed from the file name (`mime_type_for`) unless `content_type()` was called.
   - `progress(fn(sent, total))` reports body bytes written. It is called with `(0, total)` when a try starts and a retry starts again from 0.
2. **The body is read while it is written.** The engine gets `async_job::source` (a `body_source`: a length and an `open()` that makes a `body_reader`). `exchange::send_source` writes the head with the first piece, then one piece of 64 KiB per write, reading the next piece when the previous write is done, so a file of any size costs one buffer and a slow server slows the read down (back-pressure).
3. **The length is known up front** (file sizes are read when the request is made), so the body is sent with `Content-Length`, never chunked. A file that is shorter when it is sent fails that request with `error_kind::file` (the same kind `save_to` uses); one that grew is cut at the length announced.
4. **Every try opens its own reader.** A retry, the engine's retry on a dead kept-alive connection and a 307/308 redirect copy the `request_spec`, which shares the `body_source`; each pass calls `open()` again, so the body starts from its first byte. A 303 to a GET drops the source with the body.
5. **Engine.** A spec with a source runs on `download_async_http()`, the engine that already carries the disk-bound transfers, so reading a file never stalls the short calls on the shared engine.
6. **Errors come back in the response,** not as exceptions, like every other failure of `run()`: a file that cannot be read gives `error_kind::file` with the path in `error_message` and nothing is sent; mixing parts with `data()`, `json()`, `form()` or `body_file()` gives the same kind with the reason. Parts and `body_file()` are checked when the request is made (a file that is not there is found before a connection is opened).
7. **Multipart format.** The boundary is `----httpp` plus 24 random hex digits. Names and file names are quoted; `"` is written `\"` (what the server's parser reads back) and a CR or LF becomes a space. A file part with no type gets the type of its name, else `application/octet-stream`.
8. **`clone()`** copies the parts, the file path, the progress callback and a flag that says `content_type()` was called (the test `a_request_clone_carries_every_field` is the guard).
9. **Python.** `Request.field`, `file`, `file_data`, `body_file`, `progress` and the keywords `fields=`, `files=`, `body_file=`, `progress=` (`files` takes `name: path`, `(path, filename)` or `(path, filename, content_type)`). The progress callback runs on a network thread with the GIL; an exception in it does not stop the upload and `run()` raises it afterwards (like `on_retry`).
10. **Command line.** `httpp curl -F name=value`, `-F name=@file[;type=T][;filename=F]`, `-F name=<file` (the text of a file), `--form-string`, and `-#` (`--progress-bar`, a bar on stderr). `-T file` now streams; `-T -` still reads standard input whole (its size is not known up front). `-T` keeps sending `Content-Type: application/x-www-form-urlencoded` as before; curl sends none, and changing that is not part of this decision. Exit 26 is a file that cannot be read.

## Rejected alternatives

- **An `upload` class next to `download`:** it would have repeated the whole request builder (headers, auth, proxy, retry) for one different body. The builder already has all of it.
- **Chunked transfer encoding for streams of unknown length:** `-T -` and generators in Python would need it. Not done: the engine, the proxy path and many servers treat a chunked request body differently, and every use asked for so far has a file with a known size. Upgrade path: a `body_source` whose `length` is unknown and a chunk writer in `send_source`.
- **Reading the file on the shared engine's thread:** a slow disk would stall every short API call. Rejected for the same reason downloads have their own engine.
- **Making an in-memory copy of the multipart body:** the simple version, and what `-T` did. It is what this decision removes.

## Known limits (`ponytail:` in the code)

- The boundary is not checked against the content (96 random bits make a clash not worth scanning every file). Upgrade: scan the text parts and retry with a new boundary.
- The response is read only after the whole body is written. A server that answers early (413 before the body ends) and closes is reported as a write error. Upgrade: read the response while writing.
- Python generators and file objects as a body are not bound; only paths and bytes.
- `-F` and `--form-string` parts are sent in that order (all `-F`, then all `--form-string`), not in command-line order.
