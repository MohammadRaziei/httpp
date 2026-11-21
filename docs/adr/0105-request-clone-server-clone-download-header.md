# ADR-0105: `request::clone()`, `server::clone()` and `download::header(name, value)`

- **Status:** implemented in C++ (built and tested on Linux); Python binds `Request.clone()` and `Download.header()` / `headers=` (built and tested), `Server.clone()` is not bound (listed in `NOT_EXPOSED`); the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-21
- **Decided by:** the owner left the three open design questions of ADR-0093 and ADR-0101 to the implementer ("decide yourself", ADR-0072 rule)
- **Source:** `include/httpp/{client,server,download}.hpp`, `src/core/{client,server,download}.cpp`, `tests/cpp/test_clone.cpp`, `src/bindings/python/cy/transfer.pxi`
- **Follows:** ADR-0081, ADR-0093, ADR-0101

## Context

Three questions were open:

1. `request` is a move-only builder, so a prepared request cannot be used as a template (`base.timeout(5).bearer(t)`, then one variation per call) without building it again.
2. `server::clone()` was reserved by ADR-0093 and not built, because a clone needs its own engine config and that needed `server.cpp` read first.
3. `download` took credentials, proxy and TLS (ADR-0101) but no arbitrary header, so an API key in `X-Api-Key` could not be sent.

## Decision

1. **`request::clone() const`** returns an independent copy of the builder as it is now: URL, method, body, content type, headers, parameters, cookies, the settings snapshot, the streaming callbacks and the save options. `request` stays move-only; there is no copy constructor, because `clone()` is the name that says "independent" for the whole client family (ADR-0093). Callbacks are copied as `std::function` values, so what they captured stays shared. Python: `Request.clone()`.
2. **`server::clone() const`** returns a new server with a copy of the configuration: routes (and their lookup index), mounts, hooks, middleware, error and exception handlers, the compression setting, TLS, the pool and timeout settings, and the I/O thread count. The engine settings are the `applied` list of `server::impl`, replayed on each engine of the clone. It never copies sockets, threads or the running pool: the clone is not bound. It throws `std::logic_error` while the source is listening (the config is read-only only once listening, and copying under load is the case nobody can reason about); a bound server that has not started listening can be cloned.
3. **`download::header(name, value)`** sets a header for every step of the job (the size probe, the file, the checksum file), like `client::header`: the same name in any case replaces the earlier value. It is stored in the settings, so a snapshot carries it. A header goes to the checksum file only when that is on the same host and port as the URL (as the credentials do), because an API key is a credential. `Range` and `Accept-Encoding` throw `std::invalid_argument`: the download owns them (resume, identity coding), and a second value would be sent twice. Python: `Download.header()` and the `headers=` keyword (the plural, as ADR-0103 says for `header`).

## Rejected alternatives

- **Make `request` copyable:** a copy that is silently independent is the one surprise `client` and `server` (shared handles) must not share a name with; `request` owns no shared resource, but the explicit call keeps one rule.
- **Replace the config in the clone of a server (`to.cfg = make_shared<config>(...)`):** `server::impl::pool` is a reference into the config the impl was built with, so the object must be filled in place.
- **Let `server::clone()` of a listening server share the engines or copy a snapshot:** the first is a second handle (what `=` already is), the second races with routes added by another thread.
- **Repeatable `download::header` (append, like `request::header`):** a download has no per-call header list to repeat on; the session rule (replace) is the one that cannot produce a doubled header.

## Taken by default while implementing (owner to confirm)

- **`Server.clone()` is not bound in Python.** The Python `Server` keeps routes, validators, dependencies and the OpenAPI document in Python objects. A bound C++ clone would drop them without a word. A Python-level clone is a separate step; the parity test lists the name with this reason. This is a temporary difference from the rule that the two APIs are dual.
- **A handler that captured `stopper()` stops the server it was made on, not the clone.** A handler that captured state shares it.
- **The clone of a server that had its I/O threads reduced to one because the port could not be shared gets one engine.**
- **A custom download header follows a redirect to another origin,** as `request::header` does today (only `Authorization`, `Proxy-Authorization` and `Cookie` are dropped there).

## Consequences

- `auto a = base.clone().param("page", "1");` works on a request, and one configured app can be started on two ports (plain and TLS, say) from one setup function.
- `download` can fetch from an API that wants a key in a header.
- Whoever adds a field to `request::impl` must add it to `clone()`; `a_request_clone_carries_every_field` fails when one is missed.

## Verified

Built on Linux (GCC 13, Release) and run: `tests/cpp/test_clone.cpp` (8 tests; the other 91 C++ suites pass apart from what is listed below), and in Python `test_request_clone_is_an_independent_copy`, `test_header_reaches_the_server_and_the_same_name_replaces`, `test_range_and_accept_encoding_are_the_downloads_own`. The C++ tests: : a request clone is independent in both directions and carries method, body, content type, header, parameter, cookie, settings, `on_body`, `on_head` and `save_to` with `overwrite(false)`; a server clone has the routes, the after hook, middleware and the error handler, a route added to either side is not on the other, a parametric route, `max_body` and `io_threads(2)` work on the clone, cloning a listening server throws and a bound one does not; a download header reaches the file with the identity coding intact, the same name replaces, `Range` and `Accept-Encoding` throw, and the header reaches the size probe and a same-server checksum file but not another server's.

## Not verified

Windows and macOS. The whole Python suite (only `test_download_settings.py`, `test_client.py` and the parity test were run, on a wheel built from this tree with Python 3.13). The parity test has one failure that is not from this change (`client::retry` in `NOT_EXPOSED` is no longer in the C++ headers), not checked against a clean tree. `async_socks5` of the C++ suite hangs in the sandbox at `socks5h_sends_the_name_and_socks5_sends_an_address` and was left out; `path.iterdir_leaves_out_dot_and_dotdot`, `path.a_utf8_name_works` and `path.zz_cleanup` fail as they did before this change (HANDOFF 2026-11-20). Cloning a server that has TLS set (the context is shared, not copied, and was not run).

## Related

ADR-0081, ADR-0093, ADR-0094, ADR-0101, ADR-0103
