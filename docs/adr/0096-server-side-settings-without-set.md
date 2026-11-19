# ADR-0096: server-side settings have no `set_` prefix either

- **Status:** accepted
- **Decided by:** the project owner ("maybe the name can be changed altogether, `output_path` or `sorting` or `enable_sort` or whatever"), after ADR-0092 left the server side behind
- **Source:** `include/httpp/{server,file_browser,swagger,cookie,middleware,websocket}.hpp` and their sources under `src/core`, the Cython bindings (`src/bindings/python/cy/*.pxi`) and the Python layer (`server.py`, `_run.py`, `middleware.py`), `tests/`, `docs/site`
- **Follows:** ADR-0092, ADR-0094

## Context

ADR-0092 removed `set_` from the client family and left `server`, `directory`, `file_browser`, `swagger`, the middleware classes and `cookie` on `set_...`, because some bare names might clash with types inside their classes. That left the library with two spellings for one thing, depending on which side of the wire you are on.

## Decision

- Every configuration setter of those classes loses its `set_` prefix: `server.read_timeout(30)`, `file_browser.title("Files")`, `cors.allow_origins({...})`. It returns `*this` as before. The same name with no argument is reserved for a getter (ADR-0092); none of these classes has one yet.
- Three names change beyond dropping `set_`, because the bare name is taken by a type or says too little:

| before | after | why |
|---|---|---|
| `file_browser::set_view(view_mode)` | `layout(view_mode)` | `view_mode` is the type; `view` alone is vague |
| `file_browser::set_sort(sort_key)` | `sort_by(sort_key)` | it names the action, `sort_key` is the type |
| `cookie::set_same_site(same_site)` | `same_site_policy(same_site)` | `same_site` is the enum |

- Everything else is the old name without `set_`: `server` (`error_handler`, `exception_handler`, `tls`, `thread_pool`, `read_timeout`, `write_timeout`, `keep_alive_timeout`, `keep_alive_max`, `max_body`, `header_timeout`, `body_timeout`, `max_connections`, `async_timeout`, `compression`, `reuse_port`, `io_threads`, `request_rewrite`), `directory` (`index`, `show_hidden`, `show_dates`, `download_active`, `inline_active`), `file_browser` (`title`, `theme`, `accent`, `search`, `show_hidden`, `show_dates`, `max_entries`, `safe_mode`, `zip`, `powered_by`, `max_zip_entries`), `swagger` (`title`), `cookie` (`path`, `domain`, `max_age`, `secure`, `http_only`), the middleware (`cors`: `allow_origins`, `allow_origin_regex`, `allow_methods`, `allow_headers`, `allow_credentials`, `expose_headers`, `max_age`; `trusted_host`: `allowed_hosts`, `www_redirect`; `https_redirect`: `trust_forwarded_proto`; `forwarded_headers`: `trusted_proxies`, `trust_forwarded_host`; `basic_auth`: `realm`, `users`, `path_prefix`, `verifier`; `rate_limit`: `rate`, `burst`, `max_clients`) and `websocket` (`async_timeout`).
- Python follows the C++ names (`Server.read_timeout(30)`, `Server.tls(cert, key)`, `Directory.index(...)`), with the existing aliases for the names that already differed.
- Constructors whose parameter has the name of a setter call it as `this->name(...)` (`trusted_host`, `basic_auth`).

## Not renamed

- `set_cookie` and `set_header` on a response: they are actions on one response (`set_header` replaces, `add_header` appends), not configuration.
- The logging functions (`set_level`, `set_log_level`, `set_log_file`, `set_log_system`, `set_log_callback`, `set_formatter`, `set_propagate`, `set_serialized`) imitate Python's `logging` (ADR-0057), and `bar::set_progress` / `bar::set_total` is a position, not a setting.
- Python-only internals `Server.set_before_hook`, `set_after_hook`, `set_error_hook`, which have no C++ counterpart.

## Consequences

- Breaking for every user of the names above, in C++ and in Python. The version is still 0.x. Old ADRs keep the names they used then.
- Getters for these classes are still to come, one by one where they are useful.

## Related

ADR-0092, ADR-0094
