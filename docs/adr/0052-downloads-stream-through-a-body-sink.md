# ADR-0052: Downloads stream through a body sink on their own engine

- **Status:** accepted (step 2 of retiring cpp-httplib)
- **Decided by:** the project owner (retire cpp-httplib; copy from it where useful; no duplicated code)
- **Source:** `src/core/internal/async_http.{hpp,cpp}` (`body_sink`), `src/core/internal/http_exchange.{hpp,cpp}`, `src/core/download.cpp`, `tests/cpp/test_async_stream.cpp`, `tests/cpp/test_download_transfer.cpp`

## Context

A download wrote the body with cpp-httplib's content receiver. The engine we own collected the whole body in `response::body`, which is wrong for files of any size.
`download.cpp` also had two near-copies of the same transfer (`run_plain`, `run_resumable`) that differed in a few lines.

## Decision

- **`body_sink`** (engine): an `async_job` may carry a sink with `on_head(head) -> stream | skip | abort` and `on_data(chunk) -> bool`. `stream` delivers the body as sent
  (no gzip decoding, no `max_response_size`), `skip` reads and discards it but still returns the response (a redirect, an error status), `abort` or a `false` from `on_data` ends the
  exchange with `error_kind::canceled`. The next chunk is read only after `on_data` returns, which is the back-pressure. A streamed connection goes back to the pool like any other.
- **`request_spec::sink`** (policy layer): `make_job` wraps it so a redirect that is followed is always `skip`ped. Redirect classification stays in one place (`is_redirect`).
- **A second engine, `download_async_http()`:** specs with a sink run there (`engine_for`). The sink writes a file on the loop thread; on its own engine a slow disk stalls other downloads, never the
  short API calls on `shared_async_http()`. `run_blocking` checks the engine the spec runs on when it refuses to wait on the loop thread.
- **`download.cpp`:** one `file_target` (the sink: opens the file only for an accepted status, so an error page never lands in it; appends on 206, restarts on any other success; drives the progress bar for the whole
  file) and one `transfer()` used by both modes, the checksum file and the probe (`probe_size`, a HEAD). `run_plain` and `run_resumable` keep only their own policy (cleanup, `.part`, skip).
  Requests send `Accept-Encoding: identity`, and the bytes are saved exactly as the server sends them.
- `make_spec_for_url` (target from a URL, `extra_query`) is shared by `client::fetch`, `client::request` and downloads; `endpoint_of` moved to the policy layer.

## Behaviour changes

- The destination file is created when the response head is accepted, not before the request; a failed download still removes `dest_path` (plain mode) or an unusable `.part` (resume mode), as before.
- Error text for a transport failure is `request failed (connection error): <detail>`; a status failure is unchanged.
- Default timeout (no `set_timeout`) is 300 s per step, as for the client.

## Rejected alternatives

Buffering the body and writing it afterwards (memory = file size); a worker pool for the file writes (more code for a cost the dedicated engine already contains); a sink interface class (two `std::function`s match `done_fn` and need no hierarchy).

## Consequences

- Not done: per-chunk disk I/O still blocks the download loop thread. ponytail: upgrade path is handing writes to a small pool with a bounded queue (back-pressure by not reading while it is full).
- Verified on Linux, Python 3.13 (see ADR-0053 for the totals). Not verified: Windows, macOS, a transfer against a live CDN.

## Related

ADR-0003, ADR-0051
