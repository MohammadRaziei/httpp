# ADR-0081: `server` is a shared handle; `mount` takes and returns it by value

- **Status:** accepted; supersedes the first decision of ADR-0078 ("`mount` returns the server it was given")
- **Decided by:** the project owner, with two conditions: `server::stopper()` must exist to break reference cycles, and "the last handle closes the server" must be documented
- **Source:** `include/httpp/server.hpp` (`server`, `server_stopper`, `directory::mount`), `include/httpp/file_browser.hpp`, `include/httpp/swagger_ui.hpp`, `src/core/server.cpp`, `tests/cpp/test_server_lifecycle.cpp`

## Context

The wanted way to write a server is one expression:

```cpp
auto srv = httpp::directory("public").set_show_hidden(false)
               .mount(httpp::server(), "/")
               .set_tls("cert.pem", "key.pem");
```

With `server` move-only and `mount(server&)` it failed twice: a temporary cannot bind to `server&`, and `auto srv = ...set_tls(...)` copies the `server&` that `set_tls` returns, which was deleted. `responder` and `async_client` already hold a `shared_ptr<impl>`; `server` held a `unique_ptr<impl>`.

## Decision

- **`server` is a copyable handle that shares its state** (`shared_ptr<impl>`). Copies are the same server: the same routes, settings and sockets, so mounting or configuring through any handle changes all of them.
- **The server closes when the last handle is destroyed** (or by `stop()`/`shutdown()`), no longer "when the original goes out of scope". The destructor of `impl` is the one that ran before, so every loop and engine ends the same way. A thread that is inside `listen()` holds a handle of its own.
- **`mount` takes the server by value and returns it by value**, on `directory`, `file_browser` and `swagger`. A temporary works, a named server sees what was mounted, and the chain goes on.
- **`server::same_as(other)`** compares the two handles' servers, because the addresses of handles differ now.
- **`server_stopper` and `server::stopper()`.** A copyable weak handle with `stop()`, `shutdown(grace)` and `alive()`. It never keeps the server alive, and does nothing once the server is gone. A route handler that must stop the server captures this, never a `server`: a handler that holds a `server` makes the cycle server -> handler -> server and the server is never freed (tested: a handler capturing a stopper does not leak).
- **A chain on a temporary returns a handle, not a reference.** Each chain member (`set_tls`, `use`, `before`, `set_*`, 19 in all) has a `&` version that returns `server&` and a `&&` version that returns `server`. `auto& r = httpp::server().set_tls(...)` therefore does not compile, where a plain `server&` would have dangled at the end of the statement. Write `auto srv = chain...;`.

## Rejected alternatives

- **Keeping `unique_ptr` with `&`/`&&` overloads that return `server&&`.** `auto&& r = ...` dangles, and every chain member would still need two forms.
- **`directory::make_server()`.** A second name for the same result (as in ADR-0078).
- **Only documenting the dangling form.** The `&&` overloads cost one line each and make it a compile error.

## Consequences

- Source compatible for callers that ignore `mount`'s result or pass a named `server`. The type of `server` changed, so the library must be rebuilt, and a program that stored `&server` as identity should use `same_as`.
- Registering routes through two handles at the same time is as unsupported as it was for one server: the handle does not add a lock. **Not verified** beyond reading the code; configure a server from one thread before listening.
- A moved-from handle has no server: it may only be assigned to or destroyed (as before).
- Python: the Cython `Server` holds a heap `cpp_server` and passes it to `mount`, which now copies the handle (one reference count). Python does not see the handle, `same_as` or `stopper` (they are in `NOT_EXPOSED`), and Python's closing behaviour is unchanged.
- Run on Linux only; Windows and macOS were not tried.

## Related

ADR-0078 (the commands call the library), ADR-0066 (builders), ADR-0038 (`shutdown`), ADR-0017 (`NOT_EXPOSED`).
