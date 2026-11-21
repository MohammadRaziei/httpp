# ADR-0051: cpp-httplib is retired: the blocking client on the async engine, downloads through a body sink

- **Status:** accepted (merged ADR; each part keeps its own status line)
- **Merged on:** 2026-11-21, at the request of the project owner: the decisions of one subject were spread over 3 ADRs. The text of every part is kept, as a section below, in the order it was decided.

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history. Where a part and this summary disagree, this summary wins.

- **One policy module owns what to send, the engine only moves bytes:** `internal/http_exchange` (`session_settings`, `request_spec`, `run_chain`, `run_blocking`) is shared by `client`, `client::request` and `async_client`; `client.cpp` keeps only base-URL handling, the fluent builders and the sync/async calling styles.
- **Downloads stream through a `body_sink`** (`on_head` answers stream, skip or abort; `on_data` returns false to stop; the next chunk is read only after `on_data` returns, which is the back-pressure). Downloads run on their own engine, `download_async_http()`, so a slow disk stalls other downloads and never the short API calls. `download.cpp` has one `file_target` and one `transfer()`.
- **cpp-httplib and its include path are deleted;** the old `httpp::curl::request` became a thin face on `client::request` (retired later with `httpp/curl.hpp`). mbedtls stays; the trust-store logic in `tls_session.cpp` follows cpp-httplib (MIT) and its notice is in `src/third_party/README.md`.

## Parts
### The blocking client runs on the async engine and one shared policy layer

- **Status:** accepted (step 1 of retiring cpp-httplib; supersedes the "sync client on cpp-httplib" row of ADR-0003 for `httpp::client`)
- **Decided by:** the project owner (retire cpp-httplib; copy from it where useful; no duplicated code, modular OOP)
- **Source:** `src/core/internal/http_exchange.hpp`, `src/core/http_exchange.cpp`, `src/core/client.cpp`, `tests/cpp/test_client_blocking_engine.cpp`

#### Context

`client`, `client::request::run` and `client::fetch` ran on cpp-httplib, one blocking thread per request, while `request::run_async` and `async_client` ran on our engine with
their own copy of redirects, auth, cookies, proxy and TLS policy. The settings (headers, timeout, redirects, auth, proxy, verify, CA file) were kept three times, in three shapes.

#### Decision

One policy module, `internal/http_exchange`, owns everything about *what* to send; the engine (`async_http`) only moves bytes.

| Piece | Role (pattern) |
|---|---|
| `session_settings` | what a caller configures once; `make_spec(...)` builds one request from it (Builder). Used by `client`, `client::request` and `async_client`. |
| `request_spec` | one logical request, a plain value; a redirect derives the next hop from it |
| `run_chain` | runs a spec on the engine and follows redirects (the policy loop, moved out of `client.cpp` unchanged) |
| `run_blocking` | waits for `run_chain` (Adapter: sync on top of async) |

`client.cpp` is now only base-URL handling, the fluent builders and the sync/async calling styles. Nothing in it names httplib. Behaviour changes, all deliberate:

- A blocking call made on the engine's own thread (inside a `run_async` callback) returns `error_kind::other` at once instead of deadlocking (`async_http::on_loop_thread`).
- A `client` is no longer serialized by a mutex: threads may share one and calls run side by side. Only reading and writing the settings take a lock; a request keeps the settings it started with.
- Connections are pooled in the engine by (host, port, TLS, proxy), so two `client` objects to the same server share them. `client::fetch` and `set_keep_alive(false)` do not park the connection.
- `async_client::set_header` replaces a same-name default (it used to append a duplicate), as `client::set_header` always did. A per-call header still wins over a default.
- If both Basic and Bearer credentials are set, Basic is sent (the async path already did this; the old blocking path sent whichever httplib applied).

#### Rejected alternatives

Keeping a dedicated connection per `client` (needs a session id in the pool key and gives up sharing for no gain); a second adapter class around `async_client` (it adds a queue the blocking
path does not need); copying httplib's client code (it is the blocking design we are leaving, and the policy we need already exists and has tests).

#### Consequences

- Downloads and `curl.cpp` still use cpp-httplib (steps 2 and 3). `httplib_common.hpp` and the macOS Keychain link flags stay until then.
- One place for redirect, TLS, proxy and auth bugs; no thread is blocked inside a socket call.
- Verified on Linux, Python 3.13: C++ 465/465; the wheel builds and the Python suite passes (850 passed, 1 skipped: the free-threaded test). Not verified: Windows, macOS, Python 3.14t, a comparison against live public sites.

#### Related

ADR-0003 and 0004

### Downloads stream through a body sink on their own engine

- **Status:** accepted (step 2 of retiring cpp-httplib)
- **Decided by:** the project owner (retire cpp-httplib; copy from it where useful; no duplicated code)
- **Source:** `src/core/internal/async_http.{hpp,cpp}` (`body_sink`), `src/core/internal/http_exchange.{hpp,cpp}`, `src/core/download.cpp`, `tests/cpp/test_async_stream.cpp`, `tests/cpp/test_download_transfer.cpp`

#### Context

A download wrote the body with cpp-httplib's content receiver. The engine we own collected the whole body in `response::body`, which is wrong for files of any size.
`download.cpp` also had two near-copies of the same transfer (`run_plain`, `run_resumable`) that differed in a few lines.

#### Decision

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

#### Behaviour changes

- The destination file is created when the response head is accepted, not before the request; a failed download still removes `dest_path` (plain mode) or an unusable `.part` (resume mode), as before.
- Error text for a transport failure is `request failed (connection error): <detail>`; a status failure is unchanged.
- Default timeout (no `set_timeout`) is 300 s per step, as for the client.

#### Rejected alternatives

Buffering the body and writing it afterwards (memory = file size); a worker pool for the file writes (more code for a cost the dedicated engine already contains); a sink interface class (two `std::function`s match `done_fn` and need no hierarchy).

#### Consequences

- Not done: per-chunk disk I/O still blocks the download loop thread. ponytail: upgrade path is handing writes to a small pool with a bounded queue (back-pressure by not reading while it is full).
- Verified on Linux, Python 3.13 (see the part "cpp-httplib is removed" for the totals). Not verified: Windows, macOS, a transfer against a live CDN.

#### Related

ADR-0003 and the part "The blocking client runs on the async engine and one shared policy layer"

### cpp-httplib is removed

- **Status:** accepted; supersedes the "cpp-httplib for the sync client" part of ADR-0003
- **Decided by:** the project owner
- **Source:** `src/core/curl.cpp`, `CMakeLists.txt`, `src/third_party/README.md`

#### Context

After the part "The blocking client runs on the async engine and one shared policy layer" (blocking client) and the part "Downloads stream through a body sink on their own engine" (downloads) only `curl.cpp` still used `httplib.h`, a 22,600-line header compiled into the core.

#### Decision

- `httpp::curl::request` becomes a thin face on `client::request` (same engine, same policy, no state of its own).
- `src/third_party/cpp-httplib/` and `src/core/internal/httplib_common.hpp` are deleted, with the include path, `CPPHTTPLIB_MBEDTLS_SUPPORT` and the sdist entry. mbedtls stays (it is `tls_session.cpp`'s), and so do the macOS
  CoreFoundation/Security link flags (the Keychain trust store is read by `tls_session.cpp`).
- The trust-store logic in `tls_session.cpp` follows cpp-httplib (MIT); its notice is in `src/third_party/README.md`.

#### Consequences

- One HTTP implementation for server, client, async client and downloads; zero system dependencies is unchanged.
- Smaller binary and faster compile (no `httplib.h` translation units).
- Verified on Linux, Python 3.13: C++ 479/479; wheel built from scratch; Python 850 passed, 1 skipped (free-threaded only). Not verified: Windows, macOS, Python 3.14t, live sites.

#### Related

ADR-0003, the part "The blocking client runs on the async engine and one shared policy layer" and the part "Downloads stream through a body sink on their own engine"
