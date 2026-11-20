# ADR-0094: one settings mixin for `request`, `client` and `async_client`; `client::make_request(path)`

- **Status:** implemented; the points under "Taken by default" are for the owner to confirm
- **Decided by:** the project owner for: one shared declaration of the settings, no `set_` prefix and the same name for the getter (ADR-0092), the factory named `request`, setters and getters with no extra types in the way (`session_settings` stays plain data)
- **Source:** `include/httpp/client.hpp`, `src/core/client.cpp`, `src/core/download.cpp`, `src/core/internal/http_exchange.hpp`, `CMakeLists.txt` (C++17), `src/bindings/python/cy/client.pxi`
- **Follows:** ADR-0092, ADR-0093

## Context

The settings are already **one type** in the implementation: `detail::session_settings` is held by `client::impl` (behind `guarded_settings`, a mutex), by `async_client::impl` (no mutex; the comment says "configure before the first request"), by `request::impl`, and, for a moment, by `download.cpp` (which built a fresh one from two numbers for each step; it was not held by the class, see ADR-0101). They are **not one thing in the public header**: `include/httpp/client.hpp` declares the same settings three times (about 50 declarations: 20 in `client`, 11 in `request`, 18 in `async_client`, counted from the header) and `src/core/client.cpp` forwards each of them by hand. The small rules sit inside those forwarders and are repeated: `timeout(n)` with `n <= 0` means the default, `max_redirects(n)` also sets `follow` and refuses a negative number other than -1, `basic_auth` and `bearer` write several fields, `header` replaces a same-name header.

Constraints found in the code:

- The setters must return the **derived type** so a chain such as `request(url).timeout(30).json(body)` keeps working. A base class that is not a template can only return the base, and the chain stops compiling.
- The project is C++17 (`CMakeLists.txt`), so "deducing this" (C++23) is not available.
- `session_settings` is internal (`src/core/internal/`); the public header cannot name its fields.
- ADR-0092 says `request` is a builder with **no getters**, and that `header`, `basic_auth`, `bearer` and `proxy` have none on the sessions either.

## Decision

1. **A public class template (CRTP), working name `configurable<Self>`**, declares each shared setter once, and the getter where ADR-0092 has one. `client`, `async_client` and `request` derive from it, so each writes `class client : public configurable<client>`. The name is not final (see "Still open").
2. **The mixin has no state.** Each derived class says how to read and how to update its `session_settings`. Ownership (a handle for `client` and `async_client`, a plain member for `request`) and locking (the mutex of `client`) stay in the class and are not the mixin's business.
3. **Setters come in two forms**, `Self& f(...) &` and `Self f(...) &&`, so a chain on a temporary yields a value, not a dangling reference (as ADR-0081 does for `server`), and each is written once for all three classes.
4. **`session_settings` stays a plain struct with public fields.** Names follow ADR-0092 in the public API only; the struct is data and needs no accessors. The rules listed above move into small internal functions next to `session_settings` (in `http_exchange.cpp`), so each rule exists once; the mixin's inline setters call them through the hooks of point 2.
5. **Settings that are not shared stay where they are:** `keep_alive` on `client` only, `max_response_size`, `on_body`, `on_head`, `cookie` and the rest of the request data on `request` only.
6. **The factory.** `client::make_request(path)` and `async_client::make_request(path)` return a `request` that starts with the base URL joined to `path` (the rule of `client::get`) and a **snapshot** of the settings taken at that moment. The name is `make_request`. It is not `request`: Python's `Client.request(method, path, ...)` is already the send call (C++ `send`), so a C++ `request(path)` that returns a builder would mean something else under the same name in the other language, and the project keeps the names of the two languages the same. It is not `build` or `prepare` either, which do not say what comes out. (Decided by the implementer, the owner having left the naming open.) The standalone `httpp::request(url)` keeps working unchanged. Inside `class client` the name `request` refers to the member function, so the header writes `httpp::request` in full where it means the class (it already does).

## Rejected alternatives

- **A non-template base for the three classes.** Chaining stops (see Context).
- **Only the `impl` structs inherit a common base** (suggested by the owner). It puts the rules in one place, and it is compatible with this decision (point 4 can be done that way), but it does not remove the roughly 50 declarations in the public header, which was the main cost.
- **`request` inherits from `client`, or `client` from `request`.** A `request` is not a client, and the settings do not travel with inheritance.
- **`request` takes a `client&`.** A request can still be in `run_async` after the client is gone, so a reference can dangle; a copy of the settings is the same as the factory.
- **Making the settings a public struct** that each class holds as a member. It removes the mixin, and every class would again write its own forwarding setters for the chain.

## Consequences

- One declaration and one definition per shared setter and getter; adding a setting touches the mixin and `session_settings`, not three classes.
- The names do not change (ADR-0092), so the Python declarations in `declarations.pxi` and the keywords of `_apply_client_options` are not expected to change; each class is still declared on its own in Cython.
- Each class gains a base; callers rebuild, source code is compatible.
- `client::make_request(path)` is a new public function; it needs tests for: the base path join, that a later change of the client does not reach an existing request, and that a request outlives its client.

## How it is built

- `configurable<Self>` (public, `include/httpp/client.hpp`) declares the ten shared settings once: `timeout`, `connect_timeout`, `max_time`, `max_redirects`, `auto_compression`, `basic_auth`, `bearer`, `proxy`, `verify`, `ca_file`. Each setter has the `&` and the `&&` form; the getters are the same name with no argument.
- Its inline code calls `detail::settings_ref`, an exported non-template view of a class's settings, where each setting and its rule are written once (`src/core/client.cpp`). The template cannot reach `session_settings` (internal), and exporting a class template would be fragile on MSVC, so the view is plain.
- Each of the three classes holds a `detail::guarded_settings` (`session_settings` and a mutex, `src/core/internal/http_exchange.hpp`) and gives the mixin a private `settings_()` hook that returns the view. The hook is exported because the mixin's code is inline.
- `session_settings` stays a plain struct. The rules that were repeated in the forwarders are members of it, with the `set_` prefix (`set_timeout`, `set_connect_timeout`, `set_max_time`, `set_max_redirects`, `set_proxy`, next to `set_basic`, `set_bearer`, `set_default_header`), because the fields carry the plain names.
- `request` also got the `&&` form on its own setters (`method`, `header`, `param`, `data`, `json`, `form`, `content_type`, `cookie`, `max_response_size`, `on_body`, `on_head`), so a chain on a temporary is a `request` all the way: `auto r = httpp::request(url).json(body).timeout(5);`.

## Taken by default while implementing (owner to confirm)

- **The name is `configurable`.** Candidates were `with_options` and `configurable_client`. It is one rename in `client.hpp` and the three `friend` lines.
- **`request` has the getters too** (`r.timeout()`). That replaces one line of ADR-0092 ("`request` is a builder and has no getters"); one declaration serves the three classes, and a getter on a builder does no harm.
- **`header` is not in the mixin.** On a session it replaces a default of the same name; on a request it appends (repeatable). Same name, two meanings, so each class declares its own.
- **`keep_alive` stays on `client`, `max_response_size` on `request`.**
- **Locking:** all three hold a mutex now. `async_client` and `request` had none; the cost is an uncontended lock per call. A setter on an `async_client` while requests are in flight is now safe, and each request still uses the settings it started with. The header comment "configure before the first request" is kept as advice, not a rule.
- **`timeout(n <= 0)` on a request** now means the default, as on the sessions (before, the negative number was stored and then treated as the default when the request was made, so nothing visible changes).
- **The internal helpers** keep their `set_` names.
- **`download`** was not joined to the mixin here. (The first version of this line said it "holds its own `session_settings`"; it did not: it kept `max_redirects_` and `timeout_seconds_` and built a `session_settings` from them for each step, which is why it had no credentials, proxy or TLS settings at all.) Done in ADR-0101.
- **The factory is `make_request`, bound in Python** as `Client.make_request(path)` and `AsyncClient.make_request(path)`, which return an `httpp.Request`. `clone()` is bound too, as `Client.clone()` and `AsyncClient.clone()`.

## Verified

Built on Linux (GCC 13, Release). `tests/cpp/test_client_handle.cpp`: the same rules in all three classes (negative values, `-1`, `proxy` that is no proxy, `auto_compression`, `ca_file`), `client::make_request(path)` and `async_client::make_request(path)` (base path join, a path with a query, headers and credentials of the session, a snapshot that does not follow later changes and outlives the session, an IPv6 base URL), and a chain on a temporary `request`. The whole existing C++ suite passes. Python: `Client.make_request` and `AsyncClient.make_request` are tested in `tests/python/test_client.py`, and the parity test finds the mixin's names on all three classes.

## Not verified

Windows and macOS (`HTTPP_API` on the private hook and on the view are the parts that matter there). That `request(path)` is free of races when the client is changed from another thread was not stress-tested; it takes a snapshot under the lock.

## Related

ADR-0081, ADR-0089, ADR-0092, ADR-0093, ADR-0095
