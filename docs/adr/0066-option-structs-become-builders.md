# ADR-0066: The remaining option structs and free functions become builder classes

- **Status:** accepted; amends ADR-0037 (middleware), ADR-0032 (directory options) and the Swagger UI decision
- **Decided by:** the project owner ("I agree with a similar design, go ahead", after ADR-0065); the details are Claude's
- **Source:** `include/httpp/middleware.hpp`, `include/httpp/server.hpp` (`directory`), `include/httpp/swagger_ui.hpp`, `src/core/middleware.cpp`, `src/core/server.cpp`, `src/core/swagger_ui.cpp`, `src/core/file_browser.cpp`, `src/bindings/python/cy/{directory,swagger_ui,server,support}.pxi`

## Context

ADR-0065 turned the file browser into a class with chained setters. Three more places still used the old style, a plain options struct plus a free function (or a nested struct plus an overload): `cors_options`/`add_cors`, `trusted_host_options`/`add_trusted_host`, `https_redirect_options`/`add_https_redirect`, `server::directory_options` with `serve_directory(mount, dir, options)`, and `mount_swagger_ui(server&, docs_path, openapi_url, title)` with three positional strings. A struct is validated only when it is used, a positional list of strings is easy to transpose, and the library everywhere else is classes with `set_*`.

## Decision

- `httpp::cors`, `httpp::trusted_host(allowed_hosts)` and `httpp::https_redirect` with chained `set_*` and `install(server&) const`. `cors::set_allow_origin_regex` compiles the expression at the setter, `set_max_age` rejects a negative value, `trusted_host` rejects an empty list or an entry such as `*example.com` at the constructor and `set_allowed_hosts`; a refused value leaves the object unchanged. The policy is built at `install`, so one object can be installed on several servers.
- `httpp::directory(root)` with `set_index`, `set_show_hidden` and `set_download_active`, and `mount(server&, base = "/") const`. The confusing `hidden` flag (true meant "visible") is `show_hidden`, same meaning, same default (true). `server::serve_directory(mount, dir)` stays as the one-line call with the defaults and is implemented through `directory`. `directory_options` and the three-argument overload are removed. The file browser mounts its `/raw` route through `directory`.
- `httpp::swagger_ui()` with `set_docs_path` (validated at the setter), `set_openapi_url`, `set_title`, and `mount(server&) const`. `mount_swagger_ui` is removed.
- No deprecated aliases (version 0.x, as in ADR-0065 and ADR-0058). Listed as changes in the CHANGELOG.
- Python: `Directory(root, **keywords)` and `SwaggerUI(**keywords)` mirror the C++ classes (keywords = setters without `set_`, setters return the object, `mount(server, ...)`); `Server.mount_swagger_ui` is removed and the app's `docs_url` uses `SwaggerUI`. The Starlette-shaped `CORSMiddleware`, `TrustedHostMiddleware` and `HTTPSRedirectMiddleware` keep their keyword constructors and call the C++ classes. The parity test maps each new class to its Python counterpart and checks every C++ setter against a Python keyword.
- Export: member functions carry `HTTPP_API`, not the class (ADR-0065).

## Consequences

- Breaking for C++ users: `httpp::add_cors(s, o)` becomes `httpp::cors().set_...().install(s)`; `add_trusted_host(s, {{"a.com"}, false})` becomes `trusted_host({"a.com"}).set_www_redirect(false).install(s)`; `add_https_redirect(s, {true})` becomes `https_redirect().set_trust_forwarded_proto(true).install(s)`; `serve_directory(m, d, options)` becomes `directory(d)...mount(s, m)`; `mount_swagger_ui(s, a, b, c)` becomes `swagger_ui().set_docs_path(a)...mount(s)`. Python: only `Server.mount_swagger_ui` is gone.
- `serve_directory` with a mount path that lacks a leading '/' still works (it is normalised), as before; `directory::mount` is strict.

## Verification

Sandbox, Linux, Release. C++ suite 526 passed (new: cors setters refuse bad values and chain, directory settings each change the answer, directory refuses a base without '/'); Python 3.14t, wheel built from the tree: 868 passed, 4 skipped before the new file, plus `tests/python/test_builders.py` (5 passed).
Not verified: sanitizers on the new code, Windows (MSVC), macOS, Python 3.13 and 3.14 with a GIL for this change.
