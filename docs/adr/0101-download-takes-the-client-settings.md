# ADR-0101: `download` takes the network settings of `client` and `request` from `configurable`

- **Status:** implemented; the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-20
- **Decided by:** the project owner ("should we connect download to configurable too?", agreed after the gap below was shown); the details were left to the implementer
- **Source:** `include/httpp/download.hpp`, `src/core/download.cpp`, `src/core/internal/http_exchange.hpp`, `src/bindings/python/cy/{declarations,transfer}.pxi`, `tests/cpp/test_download_settings.cpp`, `tests/python/test_download_settings.py`
- **Follows:** ADR-0052, ADR-0092, ADR-0094

## Context

`download` was the one class of the client family that did not use the shared settings. It kept `max_redirects_` and `timeout_seconds_` as members and, for each step (the HEAD probe, the GET, the checksum file), built a fresh `detail::session_settings` from those two numbers (`make_transfer_spec`). So a download had **no** credentials, proxy, TLS (`verify`, `ca_file`), `connect_timeout` or `max_time`: it could not fetch a file that needs a token, or from behind a proxy, or from a server with a private CA. Every setting added to `request` since would also have missed it.

`download` is not a second kind of request. It is a request whose body streams into a file, with file semantics on top (the `.part` file, resume, SHA-256, a lock between processes, progress). The engine is the same (`run_blocking`, a `body_sink`).

## Decision

1. **`class download : public configurable<download>`.** The shared settings (`timeout`, `connect_timeout`, `max_time`, `max_redirects`, `basic_auth`, `bearer`, `proxy`, `verify`, `ca_file`) come from the mixin under the same names and rules as on `client` and `request`. `download` no longer declares `max_redirects` and `timeout` itself.
2. **A builder for one job, like `request`:** `std::unique_ptr<impl>`, move-only, the `&&` form on its own setters (`output`, `enable_progress`, `disable_progress`, `resume`, `force`, `sha256`, `sha256_url`, `max_wait`), so a chain on a temporary is a `download`. `impl` holds a `detail::guarded_settings` and the file-side fields.
3. **The settings are one snapshot per run.** `run()` and `run_async()` copy the settings into the job, and each step (probe, transfer, checksum file) builds its request from that copy. A change made after `run_async()` does not reach a job already started.
4. **Defaults of a download stay:** redirects are followed (20), `timeout` 0 means 300 s for the network and 15 s for the lock. The settings of a new download start with `follow = true`.
5. **Deleted on `download`:** `auto_compression`, `retry`, `retry_delay`, `retry_connrefused` (both the setter and the getter), declared `= delete` in the class so that the names of the mixin are hidden and a call does not compile.
   - `auto_compression`: a download always asks for identity coding, because the bytes on disk are what the server sent and a checksum is over those. A setter that is accepted and ignored would be a trap.
   - `retry` (ADR-0099): a try that fails in the middle of the body would write the file twice. It needs resume to be safe; left for a later step. (Done in ADR-0104: the sink places a 206 by its Content-Range, so a retried try does not write twice, and `retry` is not deleted any more.)
6. **Credentials go to a checksum file only if it is on the same scheme, host and port as the URL.** `sha256_url(...)` can name any URL, and sending the token of the file to another server would leak it. The redirects that are followed keep the rule of `request` (credentials are not sent to another origin).
7. **Python:** `Download` gets `connect_timeout`, `max_time`, `basic_auth`, `bearer`, `proxy`, `verify`, `ca_file`. `Download` joins `CONFIGURABLE_USERS` in the parity test; the deleted names are in `NOT_EXPOSED` with the reason.

## Rejected alternatives

- **Do it all inside `request`** (`request(url).download_to(path)`): puts `.part`, resume, locking and SHA-256 into the class for every request.
- **Keep `download` as it was and add the missing settings one by one:** a third copy of the declarations, which ADR-0094 removed.
- **Keep `download` copyable:** nothing copied it (every use chains on a temporary and calls `run`), and a copy of a builder that owns a mutex would have to snapshot; `request` is the model.
- **Accept `auto_compression` and `retry` and ignore them, or fail in `run()`:** both hide a mistake until later; a deleted name is a compile error.

## Taken by default while implementing (owner to confirm)

- **`max_time` is available on a download.** It limits the whole transfer (curl `--max-time`), which for a large file is a real cut-off; it is the same meaning as on a request, and off by default.
- **No `header(name, value)` on a download here.** Credentials, proxy and TLS were the gap; ADR-0105 adds the header.
- **The move-only change is breaking** for code that copied a `download` (none was found in this repository).
- **`auto_compression` is deleted, not a no-op;** the same for `retry`.

## Consequences

- A download behind a proxy, with a token, or with a private CA works, and a setting added to `configurable` reaches `download` without more code.
- Code that copies a `download` must move it. A call of `retry` or `auto_compression` on it does not compile.
- `download.cpp` passes one `session_settings` through the steps instead of `(max_redirects, timeout_seconds)` pairs.

## Verified

Built on Linux (GCC 13, Release). `tests/cpp/test_download_settings.cpp`: defaults, the same rules as the other classes, a chain on a temporary, `bearer` and `basic_auth` reaching the server, identity coding, that `retry` and `auto_compression` are not callable (detection in a `static_assert`), and that a credential reaches a checksum file on the same server and not one on another server. The existing download tests pass. Python: `tests/python/test_download_settings.py` and the parity test with `Download` among the users of `configurable`.

## Not verified

Windows and macOS. A download through a real proxy and over TLS with `ca_file` (the settings reach the same engine path as on `request`, which has tests of its own, but no download test sends traffic through them). `retry` with resume (not built).

## Related

ADR-0052, ADR-0092, ADR-0093, ADR-0094, ADR-0099
