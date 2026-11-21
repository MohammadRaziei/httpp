# ADR-0093: Client classes: shared handles with `clone()`, one settings mixin (`configurable`), `download` on the mixin, and `request` / `server` clones

- **Status:** implemented (merged ADR; the points marked "Taken by default" in the parts are for the owner to confirm)
- **Merged on:** 2026-11-21, at the request of the project owner: the decisions of one subject were spread over 4 ADRs. The text of every part is kept, as a section below, in the order it was decided.

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history. Where a part and this summary disagree, this summary wins.

- **`client` and `async_client` are shared handles** like `server` (ADR-0081): a copy is the same session, and `clone()` makes an independent copy.
- **One settings mixin (CRTP, named `configurable`)** declares the settings once for `request`, `client`, `async_client` and `download`, with the same name for the setter and the getter. `session_settings` stays plain data. `header` is not in the mixin: on a session it replaces, on a request it appends, so each class declares its own.
- **`client::make_request(path)`** takes a snapshot of the session settings. A request has getters too.
- **`download` is a builder for one job on the same mixin:** the shared settings (`timeout`, `connect_timeout`, `max_time`, `max_redirects`, `basic_auth`, `bearer`, `proxy`, `verify`, `ca_file`, and later `retry`) come from the mixin under the same names; each run takes one snapshot; its own defaults stay (redirects followed, 20 hops; timeout 0 means 300 s for the network and 15 s for the lock). `auto_compression` stays deleted on `download` because the bytes on disk must be what the server sent.
- **`request::clone()`** copies a builder as it is now (so a prepared request can be a template), **`server::clone()`** copies the configuration, and **`download::header(name, value)`** sets a header for every step of the job. Python binds `Request.clone()` and `Download.header()` / `headers=`; `Server.clone()` is not bound.
- Names and defaults marked "Taken by default" in the parts are still for the owner to confirm. The naming rules (no `set_`) are ADR-0092.

## Parts
### `client` and `async_client` are shared handles; `clone()` makes an independent copy

- **Status:** implemented for `client` and `async_client`; the points under "Taken by default" are for the owner to confirm
- **Decided by:** the project owner, for the handle semantics ("a copy is the same session"); `clone()` was suggested by the owner for both `client` and `server`
- **Source:** `include/httpp/client.hpp`, `src/core/client.cpp` (`client::impl`, `async_client::impl`), `src/bindings/python/cy/client.pxi`, `docs/adr/0081-server-is-a-shared-handle.md`
- **Follows:** ADR-0081 and 0092

#### Context

Today the session classes do not agree on what a copy means:

| Class | Holds its state as | Copy |
|---|---|---|
| `server` | `shared_ptr<impl>` (ADR-0081) | a copy is the same server |
| `async_client` | `shared_ptr<impl>` (requests in flight keep the state alive) | a copy is the same session |
| `client` | `unique_ptr<impl>` | deleted; move only |
| `request` | `unique_ptr<impl>` | deleted; move only |

`client::impl` holds an endpoint and the settings behind a mutex (`guarded_settings`); the code read for this ADR shows no socket or thread of its own (where a `client` keeps its keep-alive connection was not checked). A search of `src`, `include`, `tests` and `examples` found no place that stores a `client` as a member, captures it in a lambda, or chains setters on a temporary one.

Reading the header, `auto c = httpp::client("https://api.example.com").bearer(token);` does not compile: `bearer` returns `client&`, and `auto` has to copy it. This is the same failure ADR-0081 describes for `server`. Python's `Client` and `AsyncClient` are reference types: `b = a` is the same object.

#### Decision

1. **`client` becomes a copyable handle** that shares its state (`shared_ptr<impl>`), like `async_client` and `server`. A copy is the same session: the same base URL, the same settings, the same mutex.
2. **`client::clone()` and `async_client::clone()`** return a new, independent session with the same base URL and a copy of the current settings. For `async_client`, the queue and the in-flight count start empty and `max_connections` is copied. A copy made by `clone()` and its source never see each other's later changes.
3. **`server::clone()` was not built here; the part "`request::clone()`, `server::clone()` and `download::header(name, value)`" builds it.** Its name and meaning are reserved: it copies the configuration (routes, mounts, settings) of a server that is not listening, never its sockets or threads. Handlers are copied as `std::function` values, so what a handler captured stays shared. A clone needs its own engine config (the sender side holds a `weak_ptr` to the config of the server it came from), which needs reading `server.cpp` first.
4. **`request` stays move-only.** It is a one-shot builder.
5. **`client::make_request(path)` takes a snapshot** of the settings when it is called (see the part "one settings mixin for `request`, `client` and `async_client`; `client::make_request(path)`"); later changes to the client do not reach a request that already exists.

#### Rejected alternatives

- **Keep `unique_ptr` and add `&&` overloads that return a value.** It fixes chaining on a temporary, and it is smaller. It leaves `client` the only session class that is not a handle, and Python's `Client` is a reference type already.
- **A value type (a copy is an independent session).** It is possible, since a `client` owns no resource of its own, but it would make `client` the one class where copy means "independent", while `async_client`, `server` and Python mean "the same". `clone()` gives the independent copy explicitly.
- **No `clone()`.** Then the common case "a client like this one with another token" has no safe spelling.

#### Consequences

- `auto admin = base; admin.bearer(admin_token);` changes `base` as well. This is the intended meaning of a handle and must be documented in the header; the independent copy is `auto admin = base.clone();`.
- Concurrent setter calls on a shared `client` are serialized by the mutex it already has. `async_client` keeps its rule "configure before the first request"; it has no mutex today, and this ADR does not change that (see the part "one settings mixin for `request`, `client` and `async_client`; `client::make_request(path)`").
- The type of `client` changes (`unique_ptr<impl>` to `shared_ptr<impl>`), so callers rebuild; source code is compatible. The Cython `Client` holds a heap `cpp_client*` and is not expected to change.
- `server::clone()` was left unbuilt here; the part "`request::clone()`, `server::clone()` and `download::header(name, value)`" built it.

#### Taken by default while implementing (owner to confirm)

- The name is `clone()` (the alternative named in discussion was `fork()`).
- `server::clone()` is built by the part "`request::clone()`, `server::clone()` and `download::header(name, value)`" (see decision 3).
- A moved-from `client` has no session, as a moved-from `server` has none (ADR-0081): it may only be assigned to or destroyed.
- `async_client::make_request(path)` makes a request that runs on its own (`run`, `run_async`); it does not go through that session's `max_connections` queue.

#### Verified

Built on Linux (GCC 13, Release) and run: `tests/cpp/test_client_handle.cpp` (a copy is the same session and `clone()` is independent, for both classes; a chain on a temporary `client` keeps the session; a clone keeps the base URL, headers and credentials), and the whole existing C++ suite, which passes. Python: `Client.clone()` and `AsyncClient.clone()` are bound (`src/bindings/python/cy/client.pxi`, `transfer.pxi`, `httpp/_http.py`) and tested in `tests/python/test_client.py`.

#### Not verified

Windows and macOS. A listening `server` was not cloned (not built).

#### Related

ADR-0081 and 0092 and the part "one settings mixin for `request`, `client` and `async_client`; `client::make_request(path)`"

### one settings mixin for `request`, `client` and `async_client`; `client::make_request(path)`

- **Status:** implemented; the points under "Taken by default" are for the owner to confirm
- **Decided by:** the project owner for: one shared declaration of the settings, no `set_` prefix and the same name for the getter (ADR-0092), the factory named `request`, setters and getters with no extra types in the way (`session_settings` stays plain data)
- **Source:** `include/httpp/client.hpp`, `src/core/client.cpp`, `src/core/download.cpp`, `src/core/internal/http_exchange.hpp`, `CMakeLists.txt` (C++17), `src/bindings/python/cy/client.pxi`
- **Follows:** ADR-0092 and the part "`client` and `async_client` are shared handles; `clone()` makes an independent copy"

#### Context

The settings are already **one type** in the implementation: `detail::session_settings` is held by `client::impl` (behind `guarded_settings`, a mutex), by `async_client::impl` (no mutex; the comment says "configure before the first request"), by `request::impl`, and, for a moment, by `download.cpp` (which built a fresh one from two numbers for each step; it was not held by the class, see the part "`download` takes the network settings of `client` and `request` from `configurable`"). They are **not one thing in the public header**: `include/httpp/client.hpp` declares the same settings three times (about 50 declarations: 20 in `client`, 11 in `request`, 18 in `async_client`, counted from the header) and `src/core/client.cpp` forwards each of them by hand. The small rules sit inside those forwarders and are repeated: `timeout(n)` with `n <= 0` means the default, `max_redirects(n)` also sets `follow` and refuses a negative number other than -1, `basic_auth` and `bearer` write several fields, `header` replaces a same-name header.

Constraints found in the code:

- The setters must return the **derived type** so a chain such as `request(url).timeout(30).json(body)` keeps working. A base class that is not a template can only return the base, and the chain stops compiling.
- The project is C++17 (`CMakeLists.txt`), so "deducing this" (C++23) is not available.
- `session_settings` is internal (`src/core/internal/`); the public header cannot name its fields.
- ADR-0092 says `request` is a builder with **no getters**, and that `header`, `basic_auth`, `bearer` and `proxy` have none on the sessions either.

#### Decision

1. **A public class template (CRTP), working name `configurable<Self>`**, declares each shared setter once, and the getter where ADR-0092 has one. `client`, `async_client` and `request` derive from it, so each writes `class client : public configurable<client>`. The name is not final (see "Still open").
2. **The mixin has no state.** Each derived class says how to read and how to update its `session_settings`. Ownership (a handle for `client` and `async_client`, a plain member for `request`) and locking (the mutex of `client`) stay in the class and are not the mixin's business.
3. **Setters come in two forms**, `Self& f(...) &` and `Self f(...) &&`, so a chain on a temporary yields a value, not a dangling reference (as ADR-0081 does for `server`), and each is written once for all three classes.
4. **`session_settings` stays a plain struct with public fields.** Names follow ADR-0092 in the public API only; the struct is data and needs no accessors. The rules listed above move into small internal functions next to `session_settings` (in `http_exchange.cpp`), so each rule exists once; the mixin's inline setters call them through the hooks of point 2.
5. **Settings that are not shared stay where they are:** `keep_alive` on `client` only, `max_response_size`, `on_body`, `on_head`, `cookie` and the rest of the request data on `request` only.
6. **The factory.** `client::make_request(path)` and `async_client::make_request(path)` return a `request` that starts with the base URL joined to `path` (the rule of `client::get`) and a **snapshot** of the settings taken at that moment. The name is `make_request`. It is not `request`: Python's `Client.request(method, path, ...)` is already the send call (C++ `send`), so a C++ `request(path)` that returns a builder would mean something else under the same name in the other language, and the project keeps the names of the two languages the same. It is not `build` or `prepare` either, which do not say what comes out. (Decided by the implementer, the owner having left the naming open.) The standalone `httpp::request(url)` keeps working unchanged. Inside `class client` the name `request` refers to the member function, so the header writes `httpp::request` in full where it means the class (it already does).

#### Rejected alternatives

- **A non-template base for the three classes.** Chaining stops (see Context).
- **Only the `impl` structs inherit a common base** (suggested by the owner). It puts the rules in one place, and it is compatible with this decision (point 4 can be done that way), but it does not remove the roughly 50 declarations in the public header, which was the main cost.
- **`request` inherits from `client`, or `client` from `request`.** A `request` is not a client, and the settings do not travel with inheritance.
- **`request` takes a `client&`.** A request can still be in `run_async` after the client is gone, so a reference can dangle; a copy of the settings is the same as the factory.
- **Making the settings a public struct** that each class holds as a member. It removes the mixin, and every class would again write its own forwarding setters for the chain.

#### Consequences

- One declaration and one definition per shared setter and getter; adding a setting touches the mixin and `session_settings`, not three classes.
- The names do not change (ADR-0092), so the Python declarations in `declarations.pxi` and the keywords of `_apply_client_options` are not expected to change; each class is still declared on its own in Cython.
- Each class gains a base; callers rebuild, source code is compatible.
- `client::make_request(path)` is a new public function; it needs tests for: the base path join, that a later change of the client does not reach an existing request, and that a request outlives its client.

#### How it is built

- `configurable<Self>` (public, `include/httpp/client.hpp`) declares the ten shared settings once: `timeout`, `connect_timeout`, `max_time`, `max_redirects`, `auto_compression`, `basic_auth`, `bearer`, `proxy`, `verify`, `ca_file`. Each setter has the `&` and the `&&` form; the getters are the same name with no argument.
- Its inline code calls `detail::settings_ref`, an exported non-template view of a class's settings, where each setting and its rule are written once (`src/core/client.cpp`). The template cannot reach `session_settings` (internal), and exporting a class template would be fragile on MSVC, so the view is plain.
- Each of the three classes holds a `detail::guarded_settings` (`session_settings` and a mutex, `src/core/internal/http_exchange.hpp`) and gives the mixin a private `settings_()` hook that returns the view. The hook is exported because the mixin's code is inline.
- `session_settings` stays a plain struct. The rules that were repeated in the forwarders are members of it, with the `set_` prefix (`set_timeout`, `set_connect_timeout`, `set_max_time`, `set_max_redirects`, `set_proxy`, next to `set_basic`, `set_bearer`, `set_default_header`), because the fields carry the plain names.
- `request` also got the `&&` form on its own setters (`method`, `header`, `param`, `data`, `json`, `form`, `content_type`, `cookie`, `max_response_size`, `on_body`, `on_head`), so a chain on a temporary is a `request` all the way: `auto r = httpp::request(url).json(body).timeout(5);`.

#### Taken by default while implementing (owner to confirm)

- **The name is `configurable`.** Candidates were `with_options` and `configurable_client`. It is one rename in `client.hpp` and the three `friend` lines.
- **`request` has the getters too** (`r.timeout()`). That replaces one line of ADR-0092 ("`request` is a builder and has no getters"); one declaration serves the three classes, and a getter on a builder does no harm.
- **`header` is not in the mixin.** On a session it replaces a default of the same name; on a request it appends (repeatable). Same name, two meanings, so each class declares its own.
- **`keep_alive` stays on `client`, `max_response_size` on `request`.**
- **Locking:** all three hold a mutex now. `async_client` and `request` had none; the cost is an uncontended lock per call. A setter on an `async_client` while requests are in flight is now safe, and each request still uses the settings it started with. The header comment "configure before the first request" is kept as advice, not a rule.
- **`timeout(n <= 0)` on a request** now means the default, as on the sessions (before, the negative number was stored and then treated as the default when the request was made, so nothing visible changes).
- **The internal helpers** keep their `set_` names.
- **`download`** was not joined to the mixin here. (The first version of this line said it "holds its own `session_settings`"; it did not: it kept `max_redirects_` and `timeout_seconds_` and built a `session_settings` from them for each step, which is why it had no credentials, proxy or TLS settings at all.) Done in the part "`download` takes the network settings of `client` and `request` from `configurable`".
- **The factory is `make_request`, bound in Python** as `Client.make_request(path)` and `AsyncClient.make_request(path)`, which return an `httpp.Request`. `clone()` is bound too, as `Client.clone()` and `AsyncClient.clone()`.

#### Verified

Built on Linux (GCC 13, Release). `tests/cpp/test_client_handle.cpp`: the same rules in all three classes (negative values, `-1`, `proxy` that is no proxy, `auto_compression`, `ca_file`), `client::make_request(path)` and `async_client::make_request(path)` (base path join, a path with a query, headers and credentials of the session, a snapshot that does not follow later changes and outlives the session, an IPv6 base URL), and a chain on a temporary `request`. The whole existing C++ suite passes. Python: `Client.make_request` and `AsyncClient.make_request` are tested in `tests/python/test_client.py`, and the parity test finds the mixin's names on all three classes.

#### Not verified

Windows and macOS (`HTTPP_API` on the private hook and on the view are the parts that matter there). That `request(path)` is free of races when the client is changed from another thread was not stress-tested; it takes a snapshot under the lock.

#### Related

ADR-0081, 0079, 0092 and 0095 and the part "`client` and `async_client` are shared handles; `clone()` makes an independent copy"

### `download` takes the network settings of `client` and `request` from `configurable`

- **Status:** implemented; the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-20
- **Decided by:** the project owner ("should we connect download to configurable too?", agreed after the gap below was shown); the details were left to the implementer
- **Source:** `include/httpp/download.hpp`, `src/core/download.cpp`, `src/core/internal/http_exchange.hpp`, `src/bindings/python/cy/{declarations,transfer}.pxi`, `tests/cpp/test_download_settings.cpp`, `tests/python/test_download_settings.py`
- **Follows:** ADR-0051 and 0092 and the part "one settings mixin for `request`, `client` and `async_client`; `client::make_request(path)`"

#### Context

`download` was the one class of the client family that did not use the shared settings. It kept `max_redirects_` and `timeout_seconds_` as members and, for each step (the HEAD probe, the GET, the checksum file), built a fresh `detail::session_settings` from those two numbers (`make_transfer_spec`). So a download had **no** credentials, proxy, TLS (`verify`, `ca_file`), `connect_timeout` or `max_time`: it could not fetch a file that needs a token, or from behind a proxy, or from a server with a private CA. Every setting added to `request` since would also have missed it.

`download` is not a second kind of request. It is a request whose body streams into a file, with file semantics on top (the `.part` file, resume, SHA-256, a lock between processes, progress). The engine is the same (`run_blocking`, a `body_sink`).

#### Decision

1. **`class download : public configurable<download>`.** The shared settings (`timeout`, `connect_timeout`, `max_time`, `max_redirects`, `basic_auth`, `bearer`, `proxy`, `verify`, `ca_file`) come from the mixin under the same names and rules as on `client` and `request`. `download` no longer declares `max_redirects` and `timeout` itself.
2. **A builder for one job, like `request`:** `std::unique_ptr<impl>`, move-only, the `&&` form on its own setters (`output`, `enable_progress`, `disable_progress`, `resume`, `force`, `sha256`, `sha256_url`, `max_wait`), so a chain on a temporary is a `download`. `impl` holds a `detail::guarded_settings` and the file-side fields.
3. **The settings are one snapshot per run.** `run()` and `run_async()` copy the settings into the job, and each step (probe, transfer, checksum file) builds its request from that copy. A change made after `run_async()` does not reach a job already started.
4. **Defaults of a download stay:** redirects are followed (20), `timeout` 0 means 300 s for the network and 15 s for the lock. The settings of a new download start with `follow = true`.
5. **Deleted on `download`:** `auto_compression`, `retry`, `retry_delay`, `retry_connrefused` (both the setter and the getter), declared `= delete` in the class so that the names of the mixin are hidden and a call does not compile.
   - `auto_compression`: a download always asks for identity coding, because the bytes on disk are what the server sent and a checksum is over those. A setter that is accepted and ignored would be a trap.
   - `retry` (ADR-0097): a try that fails in the middle of the body would write the file twice. It needs resume to be safe; left for a later step. (Done in ADR-0097: the sink places a 206 by its Content-Range, so a retried try does not write twice, and `retry` is not deleted any more.)
6. **Credentials go to a checksum file only if it is on the same scheme, host and port as the URL.** `sha256_url(...)` can name any URL, and sending the token of the file to another server would leak it. The redirects that are followed keep the rule of `request` (credentials are not sent to another origin).
7. **Python:** `Download` gets `connect_timeout`, `max_time`, `basic_auth`, `bearer`, `proxy`, `verify`, `ca_file`. `Download` joins `CONFIGURABLE_USERS` in the parity test; the deleted names are in `NOT_EXPOSED` with the reason.

#### Rejected alternatives

- **Do it all inside `request`** (`request(url).download_to(path)`): puts `.part`, resume, locking and SHA-256 into the class for every request.
- **Keep `download` as it was and add the missing settings one by one:** a third copy of the declarations, which the part "one settings mixin for `request`, `client` and `async_client`; `client::make_request(path)`" removed.
- **Keep `download` copyable:** nothing copied it (every use chains on a temporary and calls `run`), and a copy of a builder that owns a mutex would have to snapshot; `request` is the model.
- **Accept `auto_compression` and `retry` and ignore them, or fail in `run()`:** both hide a mistake until later; a deleted name is a compile error.

#### Taken by default while implementing (owner to confirm)

- **`max_time` is available on a download.** It limits the whole transfer (curl `--max-time`), which for a large file is a real cut-off; it is the same meaning as on a request, and off by default.
- **No `header(name, value)` on a download here.** Credentials, proxy and TLS were the gap; the part "`request::clone()`, `server::clone()` and `download::header(name, value)`" adds the header.
- **The move-only change is breaking** for code that copied a `download` (none was found in this repository).
- **`auto_compression` is deleted, not a no-op;** the same for `retry`.

#### Consequences

- A download behind a proxy, with a token, or with a private CA works, and a setting added to `configurable` reaches `download` without more code.
- Code that copies a `download` must move it. A call of `retry` or `auto_compression` on it does not compile.
- `download.cpp` passes one `session_settings` through the steps instead of `(max_redirects, timeout_seconds)` pairs.

#### Verified

Built on Linux (GCC 13, Release). `tests/cpp/test_download_settings.cpp`: defaults, the same rules as the other classes, a chain on a temporary, `bearer` and `basic_auth` reaching the server, identity coding, that `retry` and `auto_compression` are not callable (detection in a `static_assert`), and that a credential reaches a checksum file on the same server and not one on another server. The existing download tests pass. Python: `tests/python/test_download_settings.py` and the parity test with `Download` among the users of `configurable`.

#### Not verified

Windows and macOS. A download through a real proxy and over TLS with `ca_file` (the settings reach the same engine path as on `request`, which has tests of its own, but no download test sends traffic through them). `retry` with resume (not built).

#### Related

ADR-0051, 0092 and 0097, the part "`client` and `async_client` are shared handles; `clone()` makes an independent copy" and the part "one settings mixin for `request`, `client` and `async_client`; `client::make_request(path)`"

### `request::clone()`, `server::clone()` and `download::header(name, value)`

- **Status:** implemented in C++ (built and tested on Linux); Python binds `Request.clone()` and `Download.header()` / `headers=` (built and tested), `Server.clone()` is not bound (listed in `NOT_EXPOSED`); the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-21
- **Decided by:** the owner left the three open design questions of the part "`client` and `async_client` are shared handles; `clone()` makes an independent copy" and the part "`download` takes the network settings of `client` and `request` from `configurable`" to the implementer ("decide yourself", ADR-0067 rule)
- **Source:** `include/httpp/{client,server,download}.hpp`, `src/core/{client,server,download}.cpp`, `tests/cpp/test_clone.cpp`, `src/bindings/python/cy/transfer.pxi`
- **Follows:** ADR-0081, the part "`client` and `async_client` are shared handles; `clone()` makes an independent copy" and the part "`download` takes the network settings of `client` and `request` from `configurable`"

#### Context

Three questions were open:

1. `request` is a move-only builder, so a prepared request cannot be used as a template (`base.timeout(5).bearer(t)`, then one variation per call) without building it again.
2. `server::clone()` was reserved by the part "`client` and `async_client` are shared handles; `clone()` makes an independent copy" and not built, because a clone needs its own engine config and that needed `server.cpp` read first.
3. `download` took credentials, proxy and TLS (the part "`download` takes the network settings of `client` and `request` from `configurable`") but no arbitrary header, so an API key in `X-Api-Key` could not be sent.

#### Decision

1. **`request::clone() const`** returns an independent copy of the builder as it is now: URL, method, body, content type, headers, parameters, cookies, the settings snapshot, the streaming callbacks and the save options. `request` stays move-only; there is no copy constructor, because `clone()` is the name that says "independent" for the whole client family (the part "`client` and `async_client` are shared handles; `clone()` makes an independent copy"). Callbacks are copied as `std::function` values, so what they captured stays shared. Python: `Request.clone()`.
2. **`server::clone() const`** returns a new server with a copy of the configuration: routes (and their lookup index), mounts, hooks, middleware, error and exception handlers, the compression setting, TLS, the pool and timeout settings, and the I/O thread count. The engine settings are the `applied` list of `server::impl`, replayed on each engine of the clone. It never copies sockets, threads or the running pool: the clone is not bound. It throws `std::logic_error` while the source is listening (the config is read-only only once listening, and copying under load is the case nobody can reason about); a bound server that has not started listening can be cloned.
3. **`download::header(name, value)`** sets a header for every step of the job (the size probe, the file, the checksum file), like `client::header`: the same name in any case replaces the earlier value. It is stored in the settings, so a snapshot carries it. A header goes to the checksum file only when that is on the same host and port as the URL (as the credentials do), because an API key is a credential. `Range` and `Accept-Encoding` throw `std::invalid_argument`: the download owns them (resume, identity coding), and a second value would be sent twice. Python: `Download.header()` and the `headers=` keyword (the plural, as ADR-0092 says for `header`).

#### Rejected alternatives

- **Make `request` copyable:** a copy that is silently independent is the one surprise `client` and `server` (shared handles) must not share a name with; `request` owns no shared resource, but the explicit call keeps one rule.
- **Replace the config in the clone of a server (`to.cfg = make_shared<config>(...)`):** `server::impl::pool` is a reference into the config the impl was built with, so the object must be filled in place.
- **Let `server::clone()` of a listening server share the engines or copy a snapshot:** the first is a second handle (what `=` already is), the second races with routes added by another thread.
- **Repeatable `download::header` (append, like `request::header`):** a download has no per-call header list to repeat on; the session rule (replace) is the one that cannot produce a doubled header.

#### Taken by default while implementing (owner to confirm)

- **`Server.clone()` is not bound in Python.** The Python `Server` keeps routes, validators, dependencies and the OpenAPI document in Python objects. A bound C++ clone would drop them without a word. A Python-level clone is a separate step; the parity test lists the name with this reason. This is a temporary difference from the rule that the two APIs are dual.
- **A handler that captured `stopper()` stops the server it was made on, not the clone.** A handler that captured state shares it.
- **The clone of a server that had its I/O threads reduced to one because the port could not be shared gets one engine.**
- **A custom download header follows a redirect to another origin,** as `request::header` does today (only `Authorization`, `Proxy-Authorization` and `Cookie` are dropped there).

#### Consequences

- `auto a = base.clone().param("page", "1");` works on a request, and one configured app can be started on two ports (plain and TLS, say) from one setup function.
- `download` can fetch from an API that wants a key in a header.
- Whoever adds a field to `request::impl` must add it to `clone()`; `a_request_clone_carries_every_field` fails when one is missed.

#### Verified

Built on Linux (GCC 13, Release) and run: `tests/cpp/test_clone.cpp` (8 tests; the other 91 C++ suites pass apart from what is listed below), and in Python `test_request_clone_is_an_independent_copy`, `test_header_reaches_the_server_and_the_same_name_replaces`, `test_range_and_accept_encoding_are_the_downloads_own`. The C++ tests: : a request clone is independent in both directions and carries method, body, content type, header, parameter, cookie, settings, `on_body`, `on_head` and `save_to` with `overwrite(false)`; a server clone has the routes, the after hook, middleware and the error handler, a route added to either side is not on the other, a parametric route, `max_body` and `io_threads(2)` work on the clone, cloning a listening server throws and a bound one does not; a download header reaches the file with the identity coding intact, the same name replaces, `Range` and `Accept-Encoding` throw, and the header reaches the size probe and a same-server checksum file but not another server's.

#### Not verified

Windows and macOS. The whole Python suite (only `test_download_settings.py`, `test_client.py` and the parity test were run, on a wheel built from this tree with Python 3.13). The parity test has one failure that is not from this change (`client::retry` in `NOT_EXPOSED` is no longer in the C++ headers), not checked against a clean tree. `async_socks5` of the C++ suite hangs in the sandbox at `socks5h_sends_the_name_and_socks5_sends_an_address` and was left out; `path.iterdir_leaves_out_dot_and_dotdot`, `path.a_utf8_name_works` and `path.zz_cleanup` fail as they did before this change (HANDOFF 2026-11-20). Cloning a server that has TLS set (the context is shared, not copied, and was not run).

#### Related

ADR-0081 and 0092, the part "`client` and `async_client` are shared handles; `clone()` makes an independent copy", the part "one settings mixin for `request`, `client` and `async_client`; `client::make_request(path)`" and the part "`download` takes the network settings of `client` and `request` from `configurable`"
