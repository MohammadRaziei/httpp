# ADR-0093: `client` and `async_client` are shared handles; `clone()` makes an independent copy

- **Status:** implemented for `client` and `async_client`; the points under "Taken by default" are for the owner to confirm
- **Decided by:** the project owner, for the handle semantics ("a copy is the same session"); `clone()` was suggested by the owner for both `client` and `server`
- **Source:** `include/httpp/client.hpp`, `src/core/client.cpp` (`client::impl`, `async_client::impl`), `src/bindings/python/cy/client.pxi`, `docs/adr/0081-server-is-a-shared-handle.md`
- **Follows:** ADR-0081, ADR-0092

## Context

Today the session classes do not agree on what a copy means:

| Class | Holds its state as | Copy |
|---|---|---|
| `server` | `shared_ptr<impl>` (ADR-0081) | a copy is the same server |
| `async_client` | `shared_ptr<impl>` (requests in flight keep the state alive) | a copy is the same session |
| `client` | `unique_ptr<impl>` | deleted; move only |
| `request` | `unique_ptr<impl>` | deleted; move only |

`client::impl` holds an endpoint and the settings behind a mutex (`guarded_settings`); the code read for this ADR shows no socket or thread of its own (where a `client` keeps its keep-alive connection was not checked). A search of `src`, `include`, `tests` and `examples` found no place that stores a `client` as a member, captures it in a lambda, or chains setters on a temporary one.

Reading the header, `auto c = httpp::client("https://api.example.com").bearer(token);` does not compile: `bearer` returns `client&`, and `auto` has to copy it. This is the same failure ADR-0081 describes for `server`. Python's `Client` and `AsyncClient` are reference types: `b = a` is the same object.

## Decision

1. **`client` becomes a copyable handle** that shares its state (`shared_ptr<impl>`), like `async_client` and `server`. A copy is the same session: the same base URL, the same settings, the same mutex.
2. **`client::clone()` and `async_client::clone()`** return a new, independent session with the same base URL and a copy of the current settings. For `async_client`, the queue and the in-flight count start empty and `max_connections` is copied. A copy made by `clone()` and its source never see each other's later changes.
3. **`server::clone()` is not built now.** Its name and meaning are reserved: it copies the configuration (routes, mounts, settings) of a server that is not listening, never its sockets or threads. Handlers are copied as `std::function` values, so what a handler captured stays shared. A clone needs its own engine config (the sender side holds a `weak_ptr` to the config of the server it came from), which needs reading `server.cpp` first.
4. **`request` stays move-only.** It is a one-shot builder.
5. **`client::request(path)` takes a snapshot** of the settings when it is called (see ADR-0094); later changes to the client do not reach a request that already exists.

## Rejected alternatives

- **Keep `unique_ptr` and add `&&` overloads that return a value.** It fixes chaining on a temporary, and it is smaller. It leaves `client` the only session class that is not a handle, and Python's `Client` is a reference type already.
- **A value type (a copy is an independent session).** It is possible, since a `client` owns no resource of its own, but it would make `client` the one class where copy means "independent", while `async_client`, `server` and Python mean "the same". `clone()` gives the independent copy explicitly.
- **No `clone()`.** Then the common case "a client like this one with another token" has no safe spelling.

## Consequences

- `auto admin = base; admin.bearer(admin_token);` changes `base` as well. This is the intended meaning of a handle and must be documented in the header; the independent copy is `auto admin = base.clone();`.
- Concurrent setter calls on a shared `client` are serialized by the mutex it already has. `async_client` keeps its rule "configure before the first request"; it has no mutex today, and this ADR does not change that (see ADR-0094).
- The type of `client` changes (`unique_ptr<impl>` to `shared_ptr<impl>`), so callers rebuild; source code is compatible. The Cython `Client` holds a heap `cpp_client*` and is not expected to change.
- `server::clone()` stays unbuilt until a real use case appears; a name that is reserved but not built costs nothing.

## Taken by default while implementing (owner to confirm)

- The name is `clone()` (the alternative named in discussion was `fork()`).
- `server::clone()` is not built. Its name and meaning stay reserved (see decision 3).
- A moved-from `client` has no session, as a moved-from `server` has none (ADR-0081): it may only be assigned to or destroyed.
- `async_client::request(path)` makes a request that runs on its own (`run`, `run_async`); it does not go through that session's `max_connections` queue.

## Verified

Built on Linux (GCC 13, Release) and run: `tests/cpp/test_client_handle.cpp` (a copy is the same session and `clone()` is independent, for both classes; a chain on a temporary `client` keeps the session; a clone keeps the base URL, headers and credentials), and the whole existing C++ suite, which passes.

## Not verified

Windows and macOS. A listening `server` was not cloned (not built). Python: `clone` is not bound; it is listed in `NOT_EXPOSED` of `tests/python/test_cpp_python_parity.py` as a temporary entry, like `on_body` (the Cython `Client` holds a heap `cpp_client*` and is unchanged).

## Related

ADR-0081, ADR-0092, ADR-0094
