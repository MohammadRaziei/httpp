# Changelog

All notable changes to httpp are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project follows
[Semantic Versioning](https://semver.org/) (while the major version is 0, a minor release may still change an API;
such changes are listed under **Changed**).

## [Unreleased]

### Changed

- **The same names in C++ and Python where there was no language reason for two (ADR-0096; breaking for C++ code).** C++: `bar::is_finished()` is `finished()`, `server::before` and `server::after` are `before_request` and `after_request`, `router::ws_routes()` is `websocket_routes()`. Python: `Client.send(...)` and `AsyncClient.send(...)` are the send call, as in C++; `Client.request` and `AsyncClient.request` still work but warn with `DeprecationWarning` and will be removed. `del` and `delete`, the `*_async` suffix and `use`/`add_middleware` stay different, with their reasons in the ADR.
- **Server-side settings lose `set_` too (ADR-0096).** `server`, `directory`, `file_browser`, `swagger`, `cookie`, the middleware classes (`cors`, `trusted_host`, `https_redirect`, `forwarded_headers`, `basic_auth`, `rate_limit`) and `websocket` name their settings without the prefix: `server.read_timeout(30)`, `file_browser.title("Files")`, `cors.allow_origins({...})`, `cookie.max_age(3600)`. Three names also change: `set_view` is `layout`, `set_sort` is `sort_by`, `set_same_site` is `same_site_policy` (the bare names are types). Python follows (`Server.tls`, `Server.thread_pool`, `Server.compression`, `Server.io_threads`, `Directory.index`, ...). `set_cookie` and `set_header` on a response, the logging functions and the Python-only `Server.set_*_hook` keep their names.
- **`client` is a shared handle with `clone()`, and the settings of `request`, `client` and `async_client` come from one mixin, `configurable<Self>` (ADR-0093, ADR-0094).** `client` (like `server` and `async_client`) is copyable: a copy is the same session, so `auto admin = base; admin.bearer(t);` changes `base`; `clone()` makes an independent copy (same base URL, a copy of the settings). The ten shared settings (`timeout`, `connect_timeout`, `max_time`, `max_redirects`, `auto_compression`, `basic_auth`, `bearer`, `proxy`, `verify`, `ca_file`) are declared once; their names and rules are unchanged, and `request` gains the getters (`r.timeout()`). Every setter of `request` also has an `&&` form, so a chain on a temporary is a `request`. `async_client` and `request` keep their settings under a mutex now, as `client` always did. The type of `client` changed (rebuild). Python is unchanged.
- **Client-side settings lose `set_` and gain getters (ADR-0092).** `client`, `async_client` and `download` name their settings like `request` does: `timeout(30)` sets and returns `*this`, `timeout()` returns the value. `set_header`, `set_timeout`, `set_basic_auth`, `set_bearer_token` (now `bearer`), `set_proxy`, `set_verify`, `set_ca_file`, `set_keep_alive` and `download::set_timeout` / `set_max_wait` are now `header`, `timeout`, `basic_auth`, `bearer`, `proxy`, `verify`, `ca_file`, `keep_alive`, `timeout`, `max_wait`. `set_follow_redirects(bool)` (sessions) and `download::follow_redirects(bool)` are replaced by `max_redirects(int)` with the rules of ADR-0089 (a session follows none by default, a `download` follows 20). New on `client` and `async_client`: `connect_timeout`, `max_time`, `max_redirects`, `auto_compression`. `timeout(n <= 0)` means the default on both. `request::auto_compression` lost its default argument. Python: `Client.header` (was `set_header`), `Download.timeout` / `max_wait` / `max_redirects` (with no argument they return the value), and `Client` / `AsyncClient` take `max_redirects`, `connect_timeout`, `max_time` and `auto_compression`; `follow_redirects=True` stays as the shorthand for 20. The server side keeps `set_...`.
- **The library's own log is the logger `"httpp"` (ADR-0091).** `set_log_level` now sets the level of that logger, so `get_logger("httpp").set_level(...)` and `set_log_level(...)` are the same, and child loggers `"httpp.*"` follow it. The internal macro and its parallel back end (`own_log_level`, `log_write`) are gone: the library logs with `HTTPP_LOG` on its own logger. `HTTPP_UTILS_ABI_VERSION` is 4. The Python API is unchanged.
- **`include/httpp/swagger_ui.hpp` is now `include/httpp/swagger.hpp`, and `directory`, `file_browser` and `swagger` derive from a new `httpp::mountable` (ADR-0088).** Change `#include <httpp/swagger_ui.hpp>` to `#include <httpp/swagger.hpp>`; nothing else in the header changed (`swagger_ui_asset` and `find_swagger_ui_asset` keep their names, they are about the UI files). `mountable` (declared in `server.hpp`) has one function, `virtual server mount(server s, const std::string& base) const`, so code can hold any of the three as one thing: `void put_on(const httpp::mountable& part, httpp::server s, const std::string& base)`. Each class keeps its own default base (`"/docs"` for `swagger`, `"/"` for the others) and stays a copyable value. Python is unchanged.
- **`httpp::request::max_redirects(n)` is the one redirect setting; `request::follow_redirects` is removed (ADR-0089).** `0`, the default, follows none (a 3xx is the answer), `n` follows up to `n` hops and one more fails with `too_many_redirects`, `-1` has no limit (also `httpp curl --max-redirs -1`), any other negative number is `std::invalid_argument` / `ValueError`. This replaces ADR-0079's "`follow_redirects()` plus `max_redirects(n)`" pair: `follow_redirects().max_redirects(0)` used to fail on the first redirect and now `max_redirects(0)` follows none. Python: `Request.follow_redirects` is gone, the helpers take `max_redirects=N` and still accept `follow_redirects=True` (= 20). `httpp curl -L --max-redirs 0` still exits with 47.
- **`httpp::request::auto_compression(bool)` (ADR-0089; Python `Request.auto_compression`, `httpp.request(..., auto_compression=False)`).** On by default (unchanged behaviour). Off: no `Accept-Encoding` is sent unless `header()` sets one, and a gzip / deflate body is returned as the server sent it.
- **`httpp_utils`, a base library below the core (ADR-0086).** Logging (`httpp/logging.hpp`, with `sformat`), `httpp/path.hpp` and `httpp/progress.hpp` now live in `libhttpp_utils` (`httpp::utils`), which needs only the standard library. A program that only logs links it without the HTTP stack, mbedtls or urlparser. `httpp::core` links it PUBLIC, so existing consumers need no change. The core library file is now `libhttpp.so` / `libhttpp.dylib` / `httpp.dll` (was `libhttpp_core`); the CMake targets keep their names. New export macro `HTTPP_UTILS_API`. **Consumers that load the DLL by hand (libspeech's `_httpp_dir.py` on Windows) must find both `httpp.dll` and `httpp_utils.dll`.** Python: the log bridge is the extension `httpp_utils_python`, and `httpp_cy` is renamed `httpp_python`; the public Python API is unchanged. New `HTTPP_UTILS_ABI_VERSION` and `httpp::utils_abi_compatible()`: A consumer built apart from the library (libspeech) can call it to fail at start when its header and the loaded library disagree. Other wheels (libspeech) link `httpp::utils`, exclude it from `auditwheel`/`delocate` repair and depend on `httpp` at run time (README, Logging).

### Added

- **`httpp curl`: `-k`/`--insecure`, `--cacert`, `-J`, `--output-dir`, `--create-dirs`, `--json`, `--fail-with-body` (ADR-0098).** `-k` and `--cacert FILE` reach an https server with a certificate of your own (`--cacert` with an unreadable file is exit code 77). `-O -J` names the file after the answer's `Content-Disposition` and never replaces one that exists (exit code 23). `--output-dir DIR` and `--create-dirs` place `-o`/`-O` files. `--json DATA` sends JSON with both JSON headers. `--fail-with-body` is `-f` that keeps the body.
- **`httpp::path::mkdirs()`, `httpp::url_filename(url)` and `httpp::response::filename()` (ADR-0098).** Every missing folder of a path (pathlib's `mkdir(parents=True, exist_ok=True)`), the file name at the end of a URL path, and the name in `Content-Disposition` (a bare name, `filename*=` not read).
- **`response::elapsed_ms` and `response::bytes_received` (ADR-0097; C++ only, not bound in Python yet).** Filled on success and on failure: how long the whole request ran, redirects included (the span `max_time()` limits), and how many body bytes of the final response arrived, before decoding. A timeout with `bytes_received == 0` never got an answer; one with more was cut off while the server was still sending. `httpp curl` now reads them for `error: timed out after N ms with M bytes received` and `-w %{size_download}` instead of timing and counting in the front end. `response` grew by two fields: rebuild consumers.
- **`client::make_request(path)` and `async_client::make_request(path)` (ADR-0094; also `Client.make_request(path)` and `AsyncClient.make_request(path)` in Python, returning an `httpp.Request`).** A `request` to `path` (joined to the base URL as `get()` does) that starts with a copy of the session's headers and settings: `c.request("/users").method("POST").json(body).run()`. Later changes to the session do not reach it, and it may outlive the session. `client::clone()` and `async_client::clone()` are new too (ADR-0093), and `Client.clone()` and `AsyncClient.clone()` in Python.
- **`httpp::request::max_time(seconds)`: one limit for the whole request (ADR-0084; Python `Request.max_time`, `httpp.request(..., max_time=N)`).** Redirects and a streamed body count; when it runs out the request ends with `error_kind::timeout` even if the server is still sending. `timeout()` is unchanged (it limits each step). **`httpp curl -m` now uses it**, so `httpp curl -m 10 parrot.live` stops after ten seconds (exit code 28); before, `-m` limited each step only.
- **`httpp curl` uses proxies: `-x`/`--proxy`, `--noproxy`, and the variables `http_proxy`, `https_proxy`, `all_proxy` and `no_proxy` (ADR-0067, ADR-0084).** It used to connect directly and ignore a proxy that curl would have used, so `httpp curl http://parrot.live` hung on a machine with `http_proxy` set. `-x ""` turns the proxy off, an unusable proxy exits with 5, and `-v` prints `* Uses proxy ...`.
- **`httpp::proxy_from_environment(target)` and `httpp::resolve_proxy(target, proxy, no_proxy)` (`httpp/proxy.hpp`; Python `httpp.proxy_from_environment(url)` and `httpp.resolve_proxy(url, proxy=None, no_proxy=None)`) (ADR-0067).** The proxy a program such as curl would pick from the environment, or `None` for direct. A request still does not read the environment on its own; pass the result to `proxy()`.
- **Stream-style logging and an inline level check (ADR-0090).** `HTTPP_LOG(log, info) << "x=" << x;` and `HTTPP_LOGF(log, info, "x=%d", x);` skip their whole argument list when the level is disabled; the printf-style methods and `sformat` are unchanged and the styles can be mixed. `logger::enabled(level)` is an inline, cached version of `is_enabled_for`. `HTTPP_UTILS_ABI_VERSION` is 2 (the `logger` object grew by one atomic): rebuild anything that includes `httpp/logging.hpp`.
- **`httpp curl` reads a URL without a scheme as `http://`, sends `User-Agent: curl/8.18.0` by default, and has `-V`/`--version` (ADR-0084).** `httpp curl parrot.live` and `httpp curl localhost:8080/x` work as in curl. `httpp curl --version` prints `httpp curl 0.9.0 compatible with curl 8.18.0`. A URL with a space or a control character exits with 3 and sends nothing. `-A` or `-H` still choose the User-Agent. 
- **`httpp curl` without a URL says what curl says and exits with 2 (ADR-0084).** Bare `httpp curl` prints `httpp curl: try 'httpp curl --help' for more information`; with other options it also prints `httpp curl: no URL specified`. It was a CLI11 `url is required` usage error.
- **`httpp::request::on_body(fn)` and `on_head(fn)`: the response body streams to a callback (ADR-0084; C++, the Python binding is not done yet).** `on_body` gets each piece as it arrives (`std::string_view`, return `false` to stop with `error_kind::canceled`), `response::body` stays empty and `max_response_size` does not apply. `on_head` sees the status and headers first and returns `false` to skip the body. The request asks for `Accept-Encoding: identity` unless you set the header. `httpp curl` writes its output this way: it shows an endless answer (`httpp curl parrot.live`) at once, keeps big answers out of memory, and stops with exit code 23 when the reader of stdout goes away.
- **`httpp::openapi::document` and `httpp.openapi.Document` (ADR-0087).** A writer for OpenAPI 3.1 documents: `doc.security_scheme("jwt", httpp::openapi::bearer("JWT"))`, `doc.security(any_of({"jwt"}))`, `doc.get("/admin").summary("Admin").security(any_of({"basic"})).response(200, "OK")`, `doc.get("/health").no_security()`, then `doc.to_json()`. It escapes every string, groups the paths, and throws `std::invalid_argument` (Python `ValueError`) for an undeclared security name, a repeated operation or operation id, a wrong parameter place and text that is not UTF-8. There is no JSON parser: a schema or a body is JSON text you give and it is put in as it is. It does not read your routes, so a C++ document can drift from the handlers (Python builds its document from the routes, ADR-0007). Python's `Server.openapi()` is written through the same class, and `/openapi.json` now serves the text kept from the first build instead of converting a dict on every request. Header `httpp/openapi.hpp` (not part of `httpp.h`).
- **OpenAPI security schemes: the Swagger UI "Authorize" button (ADR-0007, Python).** `Server(security_schemes={"basic": httpp.security.basic(), "jwt": httpp.security.bearer("JWT")}, security=["jwt"])` declares them, `security=[...]` on a route (or `APIRouter(security=...)`) says what it needs and `security=[]` marks it public. A name that was not declared raises `ValueError` when the route is registered. It documents only; nothing is checked (use `BasicAuthMiddleware` or your own check). The document is built once, so it costs nothing per request.
- **`basic_auth::set_verifier(fn)` and `BasicAuthMiddleware(verifier=fn)` (ADR-0037).** `fn(user, password)` returns whether the request may pass, so passwords can be checked against hashes (bcrypt, argon2 through your own library) or a database; httpp does not hash passwords itself. With `users` and a verifier either may accept; no users are needed when a verifier is set. An exception in the verifier answers 500 and never lets the request in.
- **`httpp::cookie` (include/httpp/cookie.hpp, ADR-0037).** One cookie for both directions: `httpp::cookie("sid", token).set_max_age(3600).set_secure(true).set_same_site(httpp::same_site::lax)`, `to_set_cookie()` and `cookie::parse_cookie_header(header)`. The rules for names, values and attributes live there once; Python's `httpp.set_cookie()` and `request.cookies` call into it. `response::set_cookie(const cookie&)` takes it. Also `server_request::cookies()` (every pair of the `Cookie` header, in order) and `server_request::basic_auth()` (the user and password of an `Authorization: Basic` header; Python `request.basic_auth()` returns `(user, password)` or `None`).
- **`basic_auth` and `rate_limit` middleware (ADR-0037; Python `BasicAuthMiddleware`, `RateLimitMiddleware`).** `httpp::basic_auth(realm).set_users({"user:password"}).set_path_prefix("/admin").install(server)` answers 401 with a `WWW-Authenticate` challenge unless the request carries a valid `Authorization: Basic`; passwords are compared in constant time. `httpp::rate_limit().set_rate(10).set_burst(20).install(server)` keeps a token bucket per client address (`client_ip`, so put `forwarded_headers` first behind a proxy) and answers 429 with `Retry-After`; its table is bounded by `set_max_clients`. In Python: `app.add_middleware(BasicAuthMiddleware, users={...})` and `app.add_middleware(RateLimitMiddleware, requests_per_second=10, burst=20)`. Both run in C++ without the GIL.
- **`httpp::file_lock` / `httpp.FileLock` (ADR-0055).** A cross-process lock on a target file: `file_lock lock(path)` waits until no other thread or process holds it, and releases it when destroyed (Python: `with httpp.FileLock(path):`). It is a lock directory `<target>.lock` with a heartbeat file, so a holder that died is taken over after `stale_after` (default 15 s); if the directory cannot be created the code goes on unlocked and `owned()` says so. `download` now uses it instead of its own private lock.

- **`httpp.bridge_logger(name)` (ADR-0080).** Sends the C++ logger `name` and every logger below it (`libspeech`, `libspeech.dsp.stft`) into Python's `logging`, each record to the Python logger of the same name (the C++ function is its `funcName`). The C++ level of every logger follows the level of its Python logger, so `logging.getLogger("libspeech.dsp").setLevel(logging.DEBUG)`, `logging.config.dictConfig` and `logging.basicConfig(level=...)` all work, and C++ still builds a line only when its level lets it through. The callback is an unserialized `callback_handler` (ADR-0074), so a Python thread that holds the GIL while it logs cannot deadlock against a library thread. `"httpp"` and the names below it are refused (they have their own bridge, ADR-0018), and so are two overlapping names. `Logger.setLevel` is wrapped for every logger once a bridge exists (before, only the `"httpp"` logger's was). Nothing changes for code that does not call it.

- **`httpp::path::write_text_atomic(data)` and `read_text(start, end)` (ADR-0055).** The first writes the text to a temporary file beside the target and `rename`s it into place, so another thread or process reads the whole old text or the whole new one, never an empty or cut file, and a failure leaves the old file untouched. `read_text` now also takes a byte range (`read_text(0, 1025)` reads at most the first 1025 bytes). `download` uses the same helpers.
- **`httpp curl` takes `-s`, `-S`, `-w`, `-O`, `-T`, `-D`, `-d @file`, `--compressed`, `--retry` and several URLs (ADR-0084).** `-s` hides the error line (`-S` brings it back), `-w FORMAT` prints `%{http_code}`, `%{content_type}`, `%{size_download}`, `%{exitcode}`, `%{errormsg}` or `%{url}` after each transfer, `-O` names the file after the URL, `-T FILE` sends a file with `PUT`, `-D FILE` writes the head of each answer, `-d @file` reads the body from a file (line breaks removed, as curl does), `--retry N` with `--retry-delay` and `--retry-connrefused` repeats after a timeout or a 408, 429, 500, 502, 503 or 504, and `--compressed` is accepted (the client always sends `Accept-Encoding: gzip, deflate`). Several URLs are fetched in turn; the first failure is the exit code. A new page, *The httpp curl command*, lists every flag and the exit codes, and the server pages use `httpp curl` in their examples.
- **`httpp::request::max_redirects(n)` and `connect_timeout(seconds)` (ADR-0079; Python `Request.max_redirects`, `Request.connect_timeout`, and the same keywords on `httpp.request`, `get`, `post`, ...).** `max_redirects` sets how many redirects `follow_redirects()` may follow before the request fails with `too_many_redirects` (default 20, `0` follows none). `connect_timeout` limits resolving, connecting and the TLS handshake on its own while `timeout` keeps limiting each write and read. A negative number is `std::invalid_argument` / `ValueError`. There is no `user_agent()` or `referer()`: `header()` already sets them.
- **`httpp curl -A`, `-e`, `--max-redirs` and `--connect-timeout` (ADR-0079).** `-A` and `-e` set the `User-Agent` and `Referer` headers unless `-H` gives one; `--max-redirs N` with `-L` fails with exit code 47 after N redirects; `--connect-timeout SECONDS` limits the connect steps. The help text of `-m` now says what it limits: each connect, write or read step, not the whole transfer.
- **`httpp curl -f/--fail` and repeated `-d` (ADR-0083).** With `-f` a status of 400 or more prints nothing and exits with 22 (and writes no `-o` file). Several `-d` are joined with `&`, as curl does, and the URL may come before or after them.
- **`httpp curl` takes `-i`, `-I`, `-v`, `-L`, `-m` and `-u` (ADR-0077).** `-i` prints the status line and the headers before the body, `-I` sends `HEAD` and prints the head only, `-v` shows the request line and the `-H` headers and the response status and headers on stderr, `-L` follows redirects, `-m SECONDS` gives up after that many seconds and `-u user:password` sends basic authentication. A head is printed as `HTTP/1.1 <status>` plus the headers (the engine speaks HTTP/1.1; there is no reason phrase).


- **`httpp server` and `httpp browse` can serve HTTPS (ADR-0076).** `--ssl-certfile FILE --ssl-keyfile FILE` (both PEM, give both) switch the command to TLS 1.2/1.3 on its one port, with the option names `httpp run` already has; the printed address says `https://`. An encrypted key takes `--ssl-keyfile-password` or the `HTTPP_SSL_KEYFILE_PASSWORD` environment variable, which keeps it out of the process list. A certificate or key that cannot be read, a key that does not belong to the certificate, a wrong password, and an encrypted key given without its password each stop the command at start-up with a message naming the file. The core already had `set_tls`; only the commands are new.

- **The `httpp` commands only call the library (ADR-0078).** `directory::mount`, `file_browser::mount` and `swagger_ui::mount` now return the `server&` they were given (a reference, no copy), so the chain goes on: `httpp::directory(p).mount(srv, "/").set_tls(cert, key)`. `server::bind(host, port)` binds without listening, `server::url()` gives `https://host:port/` (the port really bound, IPv6 in brackets), and `server::listen_until_signal(grace)` serves until SIGINT or SIGTERM (first signal drains, second stops) and puts the old handlers back. `httpp::set_log_level("info")` takes a level by name, and `httpp::banner()` / `httpp::print_banner(FILE*)` hold the mark the commands show on a terminal. `set_tls` itself says when a key is encrypted and no password was given. None of the new names is in Python: `Server.run` already does it. Source compatible; `mount`'s return type changed, so rebuild against the new headers.

- **`httpp::logging::callback_handler(fn, serialized = false)` (ADR-0074).** The default is unchanged: `fn` is called with the handler's lock held, one record at a time. With `serialized = false` it is called without the lock, so a callback that has to wait for the Python GIL cannot deadlock against a Python thread that holds the GIL while it logs through the same handler (reproduced with ctypes: it deadlocked every time, even when the callback did not block). The function may then run in several threads at once and may log again. The flag is kept in the handler's private state (new protected `handler::set_serialized`), so the vtable does not change.

- **`httpp::fetch_public_suffix_list(source = "", timeout = 30)` (Python `httpp.URL.fetch_public_suffix_list`, ADR-0072).** Downloads the Public Suffix List with httpp's own client (default address: the one liburlparser names) and loads it, so `domain` and `suffix` know multi-part suffixes such as `co.uk`. Throws `std::runtime_error` (Python `RuntimeError`) on a failed request, a status other than 200, an empty body, a web page or a list that does not parse; the loaded list is then unchanged.
- **File times can be switched off (ADR-0073).** `directory::set_show_dates(false)` (Python `Directory(show_dates=False)`) sends no `Last-Modified` and no `ETag` (it holds the time), so there is no 304 and an `If-Range` cannot be checked; every answer says `Cache-Control: no-cache`. `file_browser::set_show_dates(false)` (`FileBrowser(show_dates=False)`) also removes `mtime` from `api/list`, the Modified column and the sort by date from the page, and dates every ZIP entry 1980-01-01. `httpp server` and `httpp browse` get `--no-dates`.
- **`httpp server --hidden` and a banner (ADR-0073).** Both native servers print a small ASCII banner and the version to stderr when it is a terminal (`--no-banner` and `--log-level off` turn it off; a pipe never gets it).
- **File browser: ZIP, selection, right-click menu, credit (ADR-0032).** `set_zip(true)` (Python `zip=True`; on in `httpp browse`, `--no-zip` turns it off) lets the page select files and folders (checkboxes, a rectangle dragged with the mouse, Ctrl+A) and download them as one ZIP through `GET|POST base/api/zip`: streamed, **not compressed**, Zip64, UTF-8 names, refused with `400`, `404` or `413` (`set_max_zip_entries`, default 50000) before any byte is sent. The page has its own right-click menu (open, download, copy name, path or link, select, ZIP) and ends with a small "Powered by httpp" link to the repository (`set_powered_by(false)`, `--no-powered-by` to remove it). The ZIP route is off by default in the library.

- **Typed path parameters, one syntax in both languages (breaking).** A route path is parsed by one C++ parser (`include/httpp/route_pattern.hpp`) that the C++ router and the Python binding both use. A parameter is a whole segment written `{name}`, or `{name:int}` / `{name:float}` to match only a number: `/users/{id:int}` does not match `/users/new`, so the request goes on to the next route that fits (or a `404`). Python converts an `{id:int}` argument to `int` without an annotation, and refuses an annotation that disagrees with the path. C++ gains `req.path_param<int>("id")`, which converts and throws `std::invalid_argument` when the value is missing or not a number.

### Changed

- **BREAKING (C++): one log level type (ADR-0085).** `httpp::log_level` is gone; use `httpp::logging::level` everywhere: `set_log_level(httpp::logging::level::debug)`, and `set_log_callback` takes `(logging::level, where, message)`, which is now the level of the record as made. `logging::level` gains `off` (60: above critical, a threshold only), and `set_log_level(name)` uses `level_from_string`, so it also accepts `warn`, `fatal`, `critical`, `notset` and a number. Python and the `httpp` commands are unchanged.
- **`httpp::server` is a shared handle (ADR-0081).** Copying a `server` gives another handle to the same server (routes, settings, sockets); it closes when the last handle is destroyed. `directory`, `file_browser` and `swagger` `::mount` take the server by value and return it, so `auto srv = httpp::directory(p).mount(httpp::server(), "/").set_tls(cert, key);` compiles; a chain on a temporary returns a handle, so a reference into it cannot dangle. New: `server::same_as`, `server::stopper()` and `server_stopper` (a weak handle with `stop()`, `shutdown()` and `alive()`; capture it in a handler instead of a `server`, which would make a cycle). Source compatible for callers that ignore `mount`'s result, but the type of `server` changed: rebuild.
- **BREAKING: `swagger_ui` becomes `swagger` (ADR-0082).** `httpp::swagger("/openapi.json").set_title("T").mount(srv, "/docs")` and `httpp.Swagger("/openapi.json", title="T").mount(app, "/docs")`. The OpenAPI address is a required constructor argument, the base is the argument of `mount` (default `"/docs"`, a bad one throws before anything is mounted), and `set_docs_path`, `set_openapi_url` and `SwaggerUI(docs_path=...)` are gone.
- **`response::set_cookie(name, value, attributes)` no longer takes a raw attribute string.** It was never checked, so a name, value or attribute with a line break went straight into the header. Build the attributes with `httpp::cookie` (`set_path`, `set_max_age`, ...) and pass that; `set_cookie(name, value)` keeps its `Path=/; HttpOnly` default. A name or value that cannot be sent throws `std::invalid_argument` (also from the C++ client's `request::cookie`). `server_request::cookie(name)` now ignores spaces and tabs around a pair like Python does.
- **`httpp curl` exit codes and error lines follow curl (ADR-0083, behaviour change).** An HTTP error status is an answer, so a 404 now exits 0 and prints its body (use `-f` for the old "fail on 4xx/5xx"). A transfer that produced no answer says why on stderr (`error: connection failed (...)`, `error: timed out`, ...) instead of `status 0`, writes no `-o` file, and exits with curl's code: 3 bad URL, 7 connection, 28 timeout, 35 TLS, 47 too many redirects, 63 response too large, 1 anything else. A failed lookup is reported as 7 because the engine does not tell it apart from other connection errors.
- **`httpp curl` and `httpp download` print the body byte for byte (ADR-0077).** They no longer add a newline after it, and stdout is binary on Windows, so a downloaded image or archive written to a pipe is not corrupted.
- **`httpp/log.hpp` is merged into `httpp/logging.hpp` (ADR-0075, breaking for C++ code).** Include `httpp/logging.hpp` for `set_log_level`, `set_log_file`, `set_log_system`, `set_log_callback`, `log_level` and `log_system`; nothing else changed. The operating system handlers (`syslog_handler`, `android_handler`, `unified_log_handler`, `debug_string_handler`, `event_log_handler`, each on the platform that has it, `HTTPP_LOG_HAS_*`) are now public in the same header.
- **`httpp::url` is now liburlparser's `urlparser::url` (ADR-0072, breaking for C++ code).** The wrapper class is gone. `httpp::url::parse`, `valid()`, `is_http()` and the static codec functions are removed: use `httpp::parse_url`, `url_valid`, `url_is_http`, `url_port`, `url_scheme`, `url_host`, `url_username`, `url_password`, `url_path`, `url_query`, and `httpp::encode_component`, `decode_component`, `build_query` (the Python `URL` statics, same names). `host()` is `host_text()`, `scheme()` is `protocol()`. Constructing a url from a text with a bad port (`http://h:99999/`) now throws `std::invalid_argument` (`parse_url` returns an invalid url instead), and `str()` rebuilds the text. liburlparser is a shared library shipped in the wheel SDK together with `urlparser.h`. Python `URL` is unchanged.
- **`:name` and `<name>` are gone (breaking).** C++ wrote `/users/:id` (cpp-httplib's form) and Python also accepted `<id>`; both now fail at registration with a message that says to write `{id}`. Rename them in your routes. Anything else that looks like a parameter but is not (`{id`, `a{id}`, `{}`, an unknown type, Flask's `<int:id>`) is an error too, instead of a literal that never matches. A lone `:` is still an ordinary literal segment. `httpp._params.to_cpp_pattern` is removed.
- **`httpp server` no longer serves names that start with a dot (ADR-0073, breaking for the command).** `.env`, `.git` and the like answered 200 on every interface; they answer 404 now, and `--hidden` serves them (needed for `/.well-known/`). The library's `directory` keeps its default (dot names served, `set_show_hidden(false)` to hide them).

### Removed

- **`httpp/curl.hpp` and `httpp::curl::request` (ADR-0077, breaking for C++ code).** It was a curl-flavoured subset of `httpp::request` (`method`, `header`, `data`, `content_type`, `timeout`, `follow_redirects`) that nothing used. Use `httpp::request`, which has all of those and more. (`httpp/curl_compat.h` followed it, see the next entry.)
- **`httpp/curl_compat.h` (the libcurl-style `curl_easy_*` C API) (ADR-0083, breaking for C++ code).** A subset of 15 options that nothing in the project used and that could not keep the promise of a header swap (no auth, proxy, TLS options, cookies or `multi`; the numeric error codes differed from libcurl's). Use `httpp::request`. The README now presents `httpp curl` and `httpp::request` as the modern counterpart of curl instead of a migration path.

### Fixed

- **Percent-encoded file names on macOS (Content-Disposition, `file://` URIs, URL escaping).** The encoders asked `std::isalnum` about bytes of 0x80 and above, which on macOS (a UTF-8 locale) says yes for some of them, so a UTF-8 file name such as the Persian `.zip` name of a file browser download came out with raw lead bytes instead of `%D9%81...`. Only ASCII letters and digits are left unescaped now, whatever the locale.

## [0.9.0] - 2026-11-14

### Added

- **File browser page (ADR-0032).** Audio and video previews use the page's own controls instead of the browser's: they follow the light, dark and accent settings and add seek with buffered range, volume, speed, full screen for video, keyboard keys, and previous and next through the files of the folder. Download, Open raw and Close are icon buttons with tooltips at the top right, the dialog is a little larger, opens and closes with a short animation, and closes on a click outside, Escape or the Close button. Files get an icon by kind (image, audio, video, PDF, archive, text, other) and the list view has a Type column that can be sorted. No API change.

- **The file browser shows html and svg (ADR-0070).** "Open raw" now renders them in the tab it opens, and the dialog shows an svg as an image (html stays text there), always behind `Content-Security-Policy: sandbox; default-src 'none'; ...`, so no script runs and nothing outside the server is fetched. xml, js and unknown types stay downloads. **This changes the default:** before, html and svg were always downloads. `file_browser::set_safe_mode(true)` (Python `FileBrowser.set_safe_mode` / `safe_mode=True`, `httpp browse --safe`) brings the old behaviour back. `httpp::directory` gains `set_inline_active(bool)` (Python `Directory.set_inline_active` / `inline_active=`), which does the same for a mounted folder together with `set_download_active(true)`. A `download_active` answer (the file browser's `/raw`, or a directory with `set_download_active(true)`) now carries `Cache-Control: no-cache` and an ETag that follows how the file is sent, so changing the mode is never hidden by a browser's cached copy.

### Changed

- **`httpp server` and `httpp browse` log (ADR-0011).** They printed nothing after the first line. Now, as `httpp py run`, they write `listening on http://host:port` and one line per request (`GET /a.mp3 200`) to stderr; `--log-level` (`trace`, `debug`, `info`, `warning`, `error`, `off`) changes it, and `--log-level off` is the old silent behaviour. The library's own default (`listen()` at `warning`) is unchanged.
- **C++ names now match Python's (ADR-0062, breaking for C++ code).** `httpp::request` is the client builder, as `httpp.Request` is, and the request a server handler receives is `httpp::server_request`, as `httpp.ServerRequest` is. Before, the handler's request was `httpp::request` and the builder was `httpp::client::request` (removed, no alias). Rename `const httpp::request&` to `const httpp::server_request&` in your handlers and `httpp::client::request` to `httpp::request`. Python code is unchanged.
- **File browser API** (ADR-0065): C++ `httpp::file_browser_options` and `httpp::mount_file_browser(...)` are replaced by the class `httpp::file_browser(root).set_title(...).set_theme(file_browser::theme_mode::dark)...mount(server, "/files")`; theme, view and sort are enums (`theme_mode`, `view_mode`, `sort_key`) instead of strings, and a bad accent or `max_entries` throws at the setter. Python gets the same design: `httpp.FileBrowser(root, ...)` with chained `set_*` methods, `FileBrowser.Theme/View/Sort` enums and `mount(server, base)`; `Server.mount_file_browser(...)` is removed. `httpp browse` is unchanged.
- **Builder classes for the remaining option structs (ADR-0066, breaking for C++ code).** `httpp::add_cors`, `add_trusted_host`, `add_https_redirect` and their option structs are replaced by `httpp::cors`, `trusted_host` and `https_redirect` (chained `set_*`, then `install(server&)`); `server::directory_options` and the three-argument `serve_directory` by `httpp::directory(root).set_index(...).set_show_hidden(...).set_download_active(...).mount(server, base)` (`hidden` is now `show_hidden`); `mount_swagger_ui` by `httpp::swagger_ui().set_docs_path(...).set_openapi_url(...).set_title(...).mount(server)`. Python gains `httpp.Directory` and `httpp.SwaggerUI`; `Server.mount_swagger_ui` is removed. Bad values are refused at the setter.
- **URLs and proxies (ADR-0067, breaking).** Every C++ function that needs a URL takes an `httpp::url`, which converts implicitly from a string or literal (`client`, `async_client`, `client::fetch`, `request`, `download`, `sha256_url`); `url` gains `str()`, `username()` and `password()` (Python `URL` too). A proxy is one URL, `set_proxy("http://user:pass@127.0.0.1:3128")` in C++ and `proxy="http://user:pass@127.0.0.1:3128"` in Python, and the credentials are now sent as `Proxy-Authorization`. The `(host, port)` forms and the Python tuple are removed.
- **SOCKS5 proxy (ADR-0068).** `set_proxy("socks5h://user:pass@127.0.0.1:1080")` (or `socks5://`, local DNS) in C++ and `proxy="..."` in Python; works for http and https targets. `httpp::url` parses the two schemes and gains `is_http()` (Python `URL.is_http`); a socks URL is a proxy address and is refused as a target.
- **Client address and reverse proxies (ADR-0069).** `server_request::client_ip` / `request.client_ip` (the address of the connection, `""` when unknown). `httpp::forwarded_headers` / `ProxyHeadersMiddleware`: when the connection comes from a trusted proxy (`set_trusted_proxies`, default loopback; addresses, CIDR ranges or `*`), `client_ip` comes from `X-Forwarded-For` (first untrusted address from the right), `secure` from `X-Forwarded-Proto`, and `Host` from `X-Forwarded-Host` with `set_trust_forwarded_host(true)`.
- On a free-threaded Python the default worker pool starts at 2 threads instead of `max(8, CPUs)`, grows after a 2 ms wait and still reaches the old ceiling (ADR-0059): CPU-bound `def` handlers measured 68.2k against 58.2k req/s at one I/O thread (3.14t, 16 threads). Builds with a GIL are unchanged.

### Fixed

- **`AsyncClient` and `Request.run_async` on free-threaded Python.** The completion callback ran on the C++ network thread without pinning a thread state, so every request created and destroyed one (`PyGILState_Ensure`/`Release`), which is very slow on 3.14t: in the 0.8.1 benchmark `AsyncClient` made 33,842 req/s on 3.14 and 2,426 on 3.14t (32 tasks). The server trampolines already had this fix.
- **HTTP client connection pool (ADR-0071).** The engine kept at most 8 idle connections per origin and closed the rest, so with more than 8 requests in flight (`Client` from 16 or 32 threads on a free-threaded Python, `AsyncClient` with 32 tasks) every burst reopened its extra connections. The limit is now 256. With both fixes (`benchmarks/run.sh full` on the owner's machine, i7-13620H, governor `performance`, 32 threads or tasks), 3.14t: `Client` 28,438 to 152,873 req/s, `AsyncClient` 57,634 (after the thread-state fix alone) to 113,747; on 3.14 (GIL) the same run gives 92,811 and 61,433. Test: `async_pool.a_burst_larger_than_eight_keeps_all_its_connections_for_the_next_burst`.
- **`benchmarks/run.sh` keeps no options between runs.** The `BENCH_*` options are CMake cache variables, so `run.sh full -DBENCH_ONLY=client` made every later `run.sh full` run only the client part too. `run.sh` now drops the cached `BENCH_*` values first (`-UBENCH_*`): an option applies to the run that passes it.

## [0.8.1] - 2026-11-11

### Added

- The file system module is part of `httpp::path` (ADR-0058): `stat`, `lstat`, `resolve`, `is_relative_to`, `iterdir`, `rmdir`, `is_executable` are members next to `exists`, `mkdir`, `unlink`, `rename`; there is no separate `httpp::fs` any more. `mkdir()` is now true only when this call created the directory.
- **`httpp/logging.hpp` is public** (ADR-0057): the Python-shaped logging module httpp logs through (`get_logger`, `logger`, `formatter`, `stream/file/callback/null_handler`, your own `handler` subclass) is installed and exported. The "httpp" logger can take extra handlers. Python keeps using its standard `logging`.
- **`set_log_system`** (ADR-0056, C++ and Python): send the library log to the operating system's log: syslog, logcat, Apple unified logging, Windows debugger output or the Event Log (`"native"` picks the platform's usual one). Ported from libstb.
- **`httpp::path`** (ADR-0055, `httpp/path.hpp`): a pathlib-shaped path type for C++ (`/`, `name`, `stem`, `suffix`, `parent`, `exists`, `open`, `read_text`, `path::cwd()`, `as_uri`, ...). The public API that names a file takes it (strings still convert): `download`, `sha256_file`, `set_log_file`, `set_ca_file`, `set_tls`, `mount_file_browser`, `response::file`. In Python every path argument now also takes a `pathlib.Path`.

### Fixed

- **No more `std::filesystem` anywhere in the library or the CLI** (ADR-0054): an internal `httpp::fs` module (merged into `httpp::path` by ADR-0058) (POSIX calls, the wide Win32 API on Windows, UTF-8 paths) is used by downloads, the file browser, static files, the logging module, TLS certificate loading, `sha256_file` and the CLI. This removes the macOS 10.13 build break and five private copies of the same operations. On Windows, non-ASCII file names now work in all of them.
- **macOS wheel build:** the internal logging module's `file_handler` took a `std::filesystem::path`, which the macOS 10.13 target does not have (CI failed in `log.cpp`). It takes a UTF-8 `std::string` now (converted to UTF-16 on Windows, so a non-ASCII log path works there too), and `tests/python/test_portability_sources.py` fails on any `std::filesystem` outside an `_WIN32` block.
- **Python log bridge at interpreter exit:** the C++ trampoline checks `Py_IsInitialized()` before taking the GIL and an `atexit` hook hands the log back to stderr, so a library thread that logs late cannot reach a dying interpreter (the same fix libstb made for its bridge).

### Changed

- **cpp-httplib is removed** (ADR-0052, ADR-0053). Downloads now stream through a body sink on a dedicated engine (the destination file is created only for an accepted status; a Range reply of 200 restarts the file), `httpp.curl` is a thin face on `client.request`, and the vendored `httplib.h` is deleted. No public API change.
- **The blocking client runs on the async engine** (ADR-0051): `client`, `client::request::run` and `client::fetch` share one policy layer with `async_client` (redirects, auth, cookies, proxy, TLS). A `client` can now be shared between threads without taking turns, connections are pooled across clients, a blocking call from an async callback returns an error instead of deadlocking, and `async_client::set_header` replaces a same-name default. cpp-httplib now serves only downloads and the `curl` helper.
- **Logging no longer uses the vendored AixLog** (ADR-0050): the library logs through a small internal module ported from libstb (Python-shaped loggers, handlers and formatter, no dependency, no patch). `set_log_level`, `set_log_file`, `set_log_callback` are unchanged in C++ and Python; the default line is now `time [LEVEL] httpp (function): message`.
- The Cython module `httpp_cy.pyx` is split into `cy/*.pxi` files (textual includes, still one module and one extension; ADR-0049). No behaviour change.

### Added

- **WebSocket routes on routers and `Depends` on WebSocket routes** (ADR-0048): `router::route_ws` and `@router.websocket(...)` join prefixes like HTTP routes; a Python WebSocket handler may use `Depends` (sync, async, `yield` cleanup, sub-dependencies). A failing dependency refuses the upgrade (`HTTPException` status, or 422); a dependency may take `websocket`.
- **WebSocket close 1001 on shutdown** (ADR-0043): `server.shutdown()` and SIGTERM now send each open WebSocket a close frame with code 1001 and wait for the peer's answer (up to the grace period) instead of dropping the connection; a handler sees a clean close.
- **Dead `--workers` are restarted** (ADR-0044): the supervisor starts a new worker at once the first time and with a growing wait after that (0.5, 1, 2, 4 seconds); a worker that keeps dying right after it starts stops the group with exit code 1. Workers also end when the supervisor is killed, even with SIGKILL (Linux).
- **`ETag`, `Last-Modified` and conditional requests for mounted files** (ADR-0045): `If-None-Match` and `If-Modified-Since` give 304, `If-Match` and `If-Unmodified-Since` give 412, `If-Range` decides whether a `Range` is honoured. This covers `serve_directory` and the file browser's raw route.
- **`httpp.testclient.TestClient`** (ADR-0042): a Starlette-shaped client for testing your app. It serves the app on a free loopback port of its own and goes through httpp's client, so tests meet the real routing, middleware and error paths. Cookies, redirects, `base_url`/`Host`, `raise_server_exceptions`, startup and shutdown hooks (`with TestClient(app)`). Also `files=` (multipart) and `websocket_connect` (ADR-0046). Not yet: an in-memory mode.

### Fixed

- A 304 answer no longer carries `Content-Length: 0`.
- Python's `file()`, `FileResponse` and `TestClient(files=)` choose the Content-Type with the same table as mounted directories (ADR-0047), so `ico`, `gz`, `wav` and `yaml` no longer differ and nothing depends on the operating system's MIME registry. `yaml` and `yml` are now `application/yaml`.
- The async client no longer leaks the exchanges that were still waiting when the client was destroyed (a reference cycle, ADR-0047).

### Changed (breaking)

- The command line is a native executable now (ADR-0041): `httpp server`, `httpp browse`, `httpp download`, `httpp curl` and `httpp --version` run without Python and without the GIL (new `--shutdown-timeout` on
  `server` and `browse`). The Python commands moved behind `py`: **`httpp run ...` is now `httpp py run ...`, and `httpp path ...` is now `httpp py path ...`** (`python -m httpp ...` is the same program as `httpp ...`, so it is `python -m httpp py run ...` too). The old spelling
  fails with exit 2 and says what to type. Everything after `py` goes to Python untouched.
- **The wheel keeps its C++ parts in `httpp/sdk/`** (`sdk/bin/httpp`, `sdk/include/`, `sdk/lib/`, CMake config in `sdk/lib/cmake/httpp/`; on Windows the DLL is in `sdk/bin`, next to `httpp.exe`): the shape of an SDK install prefix. `httpp.get_include_dir()`, `get_lib_dir()` and `get_cmake_dir()` return the new places, so code that uses them is unaffected; anything that hard-coded `httpp/lib` or `httpp/include` must change. `find_package(httpp)` also works with `CMAKE_PREFIX_PATH` set to that `sdk` folder.
- The native executable is inside the package (`httpp/sdk/bin/httpp`, `httpp.exe` on Windows); the `httpp` console script is `httpp_cli.py` (a module next to the package, so a native command does not import it: 25 ms to start, not 80), which finds it (restoring its execute bit if an installer dropped it) and runs it. `python -m httpp ARGS` is the same program as `httpp ARGS`
  (it calls the launcher). C++: CMake target `httpp_cli` (`-DHTTPP_BUILD_CLI=OFF` to skip), installed to `bin/` with the SDK. `HTTPP_PYTHON` names the interpreter the executable starts for `py` when it is run on its own.

### Removed

- `httpp install` and `httpp uninstall` are gone (they were never released); `httpp path` (`--include-dir`, `--lib-dir`, `--cmake-dir`) is the only SDK helper.

### Added

- C++: `server::use(before, after)` for middleware next to the application's own hooks, `httpp::add_cors` and `httpp::add_trusted_host` (`httpp/middleware.hpp`) (ADR-0037).
- Python: `Server.add_middleware(CORSMiddleware | TrustedHostMiddleware | GZipMiddleware, **options)`, shaped like Starlette's; the work runs in C++ without the GIL.
- C++: `httpp::mount_file_browser(server&, base, root, file_browser_options)`, a read-only file browser (list, navigate, preview, download; theme, accent, filter, hidden files, entry limit) with its own
  embedded page and no dependencies (ADR-0032).
- Python: `Server.mount_file_browser(base, root, **options)` (same options as C++) and the `httpp browse [DIRECTORY]` command.
- C++: `server::directory_options` and `serve_directory(mount, dir, options)`: `index`, `hidden` and `download_active` (nosniff, attachment for active types). The defaults keep the old behaviour.

- TLS for the server (ADR-0040): C++ `server::set_tls(cert, key, password)`, `request::secure` / `scheme()`, `add_https_redirect`; Python `Server.set_tls`, `request.secure` / `request.scheme`, `HTTPSRedirectMiddleware`,
  `run(ssl_certfile=, ssl_keyfile=, ssl_keyfile_password=)` and `httpp py run --ssl-certfile --ssl-keyfile --ssl-keyfile-password` (also with `--workers` and `--reload`). https and wss; TLS 1.2/1.3; no client certificates.
- Python: `httpp py run --reload` (`--reload-dir`, `--reload-pattern`, `run(reload=True, ...)`): restarts the app when a source file changes, development only (ADR-0039).
- C++: `server::shutdown(grace)`; Python: `Server.shutdown(grace)`, `Server.shutdown_timeout`, `run(shutdown_timeout=...)` and `httpp py run --shutdown-timeout` (ADR-0038).

### Fixed

- Server: a NUL byte in a path under a mounted directory is refused.
- Server: a late answer through the `after` hook, error handler or compression no longer reads the server object after the server is destroyed, and a refused job runs on the current thread
  instead of using a moved-from response (ADR-0035). No API change.

### Changed

- Ctrl+C and SIGTERM in `httpp py run` / `Server.listen` now drain: requests in progress finish (answered with `Connection: close`) within `shutdown_timeout` (default 10 s) instead of being cut; a second Ctrl+C cuts at once (ADR-0038).
- Server: the method and target are compared with string literals through `std::string_view` (no `strlen`; about 0.8 percent fewer instructions per request, ADR-0033 addendum). No API change.
- Server: the 16 KiB read buffer of a connection is no longer zero-filled when the connection is made (ADR-0036). No API change.
- Server: with several I/O threads (`set_io_threads`), new connections go to the I/O thread with the fewest connections instead of wherever the kernel's `SO_REUSEPORT` hash put them (ADR-0034). No API change.
- Server: about 23 percent fewer instructions per `route_io` request (ADR-0033, items 9 to 12): no shared reference count per answer when no hook or compression is set, the response head
  is written without `strlen`, responses are moved once instead of once per layer, a request answered on the spot is not entered in the 504 list. One visible detail: the 504 deadline of
  an unanswered asynchronous request starts when its handler returns, not at arrival. No API change.

## [0.8.0] - 2026-11-01

WebSocket in the C++ core and in Python, checked with the Autobahn conformance suite, and a benchmark suite built by CMake that
now sets httpp next to Drogon. One public C++ type changed: `swagger_ui_asset` (see the first entry).

- **Autobahn result** (second run, after [ADR-0029](docs/adr/0029-incremental-utf8-validation.md)): no FAILED case, 589 OK, 7 NON-STRICT and 6 INFORMATIONAL.
  The `permessage-deflate` cases (12 and 13) are left out on purpose. The NON-STRICT cases (3.2, 3.3, 4.1.3, 4.1.4, 4.2.3, 4.2.4, 5.15) are behaviour the RFC leaves open.
- **Build**: `embed_files(... COMPRESS)` embeds gzip-compressed bytes (same bytes on every build), with `<name>_raw_size` and `<name>_is_gzip`; `-DHTTPP_EMBED_COMPRESS=OFF` turns it off. Swagger UI uses it: `libhttpp_core.so` is about 1.3 MB smaller, and `/docs` files are sent as stored (`Content-Encoding: gzip`, `Vary: Accept-Encoding`) to clients that accept gzip and inflated for the others. **Changed:** `httpp::swagger_ui_asset::data` holds the stored bytes (gzip unless built with `HTTPP_EMBED_COMPRESS=OFF`); new fields `raw_size` and `gzip`. See [ADR-0031](docs/adr/0031-compressed-embedded-files.md).
- **WebSocket (RFC 6455) in the C++ core**: `server::route_ws(path, ws_handler)` with a `websocket` handle (`send_text`, `send_binary`, `ping`, `close`,
  `pause_reading`, `buffered_amount`). Callbacks run on the I/O thread; sends are thread-safe and the send queue is bounded (`max_buffered`, `status::full`,
  `on_drain`). Text, binary, fragmentation, ping/pong, close handshake, UTF-8 and size limits, optional keep-alive pings; no extensions (no `permessage-deflate`).
  The engine got a generic upgrade "tunnel". See [ADR-0026](docs/adr/0026-websocket.md).
- **WebSocket in Python**: `@app.websocket("/ws/{room}")` with an `async def` handler taking `websocket` (`accept`, `receive*`, `async for`, `send_*`, `ping`, `close`,
  `WebSocketDisconnect`). One task per connection on the lane's loop; the I/O thread fills a per-connection mailbox without the GIL. Receiving is bounded (`max_queued`, the socket
  is not read past it) and `await send_*` waits when the send queue is full. Browsers' `Origin` must match the host unless `origins=` says otherwise. The C++ handshake is now
  asynchronous (`ws_acceptor`, `websocket::user()`, `on_close` exactly once per accepted upgrade). See [ADR-0027](docs/adr/0027-websocket-python-and-async-handshake.md).
- WebSocket text messages are checked for UTF-8 as the bytes arrive: a bad byte closes with 1007 at once (also in the middle of a frame or a fragment), instead of at the end of the message. First Autobahn run: no FAILED case; this was the only real finding ([ADR-0029](docs/adr/0029-incremental-utf8-validation.md)).
- **Benchmarks are a CMake project** (`cmake -S benchmarks -B build-bench`, targets `bench`, `bench_quick`, `bench_check`; the Makefile is gone). New: a WebSocket echo benchmark with its own load generator (`ws_load`), a memory table for many idle WebSocket connections, and Drogon (fetched and built by CMake) next to the httpp C++ API ([ADR-0030](docs/adr/0030-benchmarks-built-by-cmake.md)).
- Autobahn runs as a job of the orchestrator, after its clean-up of the `__beta__` release ([ADR-0028](docs/adr/0028-autobahn-called-by-the-orchestrator.md)).
- Fixed a data race with several I/O threads (`set_io_threads`, Python `io_threads`): the cached `Date` header text was shared by all of them, so two threads could
  write and read it together. ThreadSanitizer reported it, and in the benchmarks two runs out of about 150 lost the server (a round with zero answers). The cache is now per thread.
- Answers from other threads are batched into one Asio post per burst, which cuts the cost of the thread hop.
- An `async def` handler that cannot suspend (no `await`, `async with` or `async for`, no `Depends`; Python 3.12+) runs on the I/O thread when the
  loop thread is asleep and no `def` handler is running, so it skips the hop to the asyncio loop: 54.7k to 71.5k req/s on the `hello` benchmark
  (2 cores, one box, interleaved). Handlers that await are unchanged. See [ADR-0023](docs/adr/0023-inline-async-handlers.md).
- `async def` and `route_async` / `route_io` requests allocate about 8 fewer objects (30 to 22 per request): `responder` keeps its state in plain members, and that state is released on the I/O thread with the answer. About 4 percent more req/s and 8 percent less CPU for an awaiting `async def`.

### Fixed
- **A failing timing assertion could hang CI for ever.** The `worker_pool` gate tests returned early when an assertion failed, the gate stayed closed and the
  pool's destructor joined blocked threads without end (a 14 minute macOS step). The tests now open the gate from an RAII guard, wait in bounded loops, and the
  CMake workflow runs `ctest --timeout 900`. The growth test no longer depends on a tight wall-clock bound (`help_after` 10 ms, limit 250 ms).
- WebSocket tests poll with a timeout instead of sleeping for a fixed time.

### Changed
- **Less work per request in the engine ([ADR-0033](docs/adr/0033-engine-per-request-cost.md))**: the Asio reactor runs without locking (sockets and timers were already
  used on the loop thread only); an answer made on the loop thread (`route_io`, an inline handler) is sent at once instead of going through the poster; connection timers
  are armed lazily (one deadline per connection, no timer work per keep-alive request); the response head is built without temporaries. About 27 percent fewer
  instructions per `route_io` request (counted with callgrind: 25.5k to 18.3k; wall-clock gain not measured on a many-core machine). ThreadSanitizer is clean on the whole C++ suite.
- The 504 backstop for async requests keeps one list per I/O thread instead of one shared list under one mutex, so I/O threads no longer contend on it (`route_io` and Python `async def` with several I/O threads).
- **`keep_alive_max` (C++ `set_keep_alive_max`, `Server(keep_alive_max=...)`) now defaults to 1000 and `0` means no limit.** It defaulted to 100, and `0` meant 1.
- **Defaults from the first 16-thread measurement ([ADR-0025](docs/adr/0025-measured-defaults-on-16-threads.md))**: `Server(io_threads=None)` now
  means `min(4, CPUs // 4)` with a GIL (1 on a free-threaded Python and on Windows); the inline `async def` path of ADR-0023 is on by default only
  with 2 CPUs or fewer (`Server(inline_async=True/False)` forces it): on a machine with cores to spare it was 20 percent slower at saturation.
- **`set_thread_pool(base, max, grow_after)`** (Python: `grow_after_ms`): how long a job waits for a free worker before the pool grows. A step adds one thread per 500 us of that wait
  (so one thread with the default wait, as before).
- With a GIL the worker pool starts with one thread and grows after a 10 ms wait (the maximum is unchanged): on 16 threads one worker gave 16.7k req/s on
  a CPU-bound `def` handler, sixteen 10.6k (ADR-0025).

### Added
- Benchmark suite: JSON and FastAPI rows, `io_threads` 1, 2, 4, 8 for Python too, library versions and compiler in the header, standard deviation, a fixed-rate
  latency phase (p99.9 at half of each maximum), efficiency cores for the load generator.
- **`server::set_io_threads(n)`, `Server(io_threads=N)` and `httpp run --io-threads N`**: several I/O threads share the port (one engine and
  one acceptor each, `SO_REUSEPORT`; not on Windows, which keeps one). On a free-threaded Python every I/O thread also gets its own asyncio
  loop thread, so `async def` handlers use several cores; with a GIL there is still one loop. Default 1. See
  [ADR-0024](docs/adr/0024-io-threads-and-lanes.md). Not yet measured on a many-core machine: `benchmarks/Makefile`.
- **`benchmarks/Makefile`** (`make bench`): builds venvs for 3.13, 3.14 and 3.14t, the C++ benchmark server and `oha`, then compares
  uvicorn, httpp `def` / `async def` / `await` and the C++ API at several I/O thread counts into one text file.
- **`server::set_body_timeout(seconds)` and `Server(body_timeout=...)`**: the whole request body must arrive within this time after
  its head (default 120 s, `0` = no limit). Before, a body dripped one byte at a time was held only by the per-read `read_timeout`.
- **`server::set_compression(min_size, level = 1)` and `Server(compression=..., compression_level=...)`**: gzip for text answers
  (`text/*`, JSON, XML, JavaScript, SVG) when the client sends `Accept-Encoding: gzip`. Off by default. It runs after the hooks, adds
  `Vary: Accept-Encoding`, and leaves files, streams, partial answers, already encoded bodies and bodies it cannot shrink alone. On a
  `route_io` answer the work moves to a worker, never the I/O thread. Uses the vendored deflate: no new dependency.
- **Routers**: `httpp::router` and `server::include(router, prefix)` in C++, `APIRouter` and `Server.include` (alias `include_router`) in
  Python. Same rules in both: outer prefix, the router's own prefix, then the path; `/` under a prefix is the prefix. Nesting works.
  No request-time cost: prefixes are joined when the router is included.
- **`response::after_send`** (C++) and, in Python, the injected **`response`** argument and **`BackgroundTasks`**: a job that runs on a
  worker thread after the answer was written, for work the client should not wait for. The injected `response` adds a status,
  headers and cookies to a handler's answer. A throwing job is logged and ignored.
- **Forms and uploads**: `request::form()` in C++ (`form_data`, `upload_file`) parses urlencoded and `multipart/form-data` bodies, only when a
  handler asks. Python: `request.form()` (a malformed body answers 400), and FastAPI-style `Form()`, `File()` and `UploadFile` parameters
  with 422 errors and OpenAPI request bodies. `percent_decode` and `parse_query` moved to one internal header.
- `ServerRequest.header()`, `.query_param()`, `.path_param()` and `.cookie()`: the lookups the C++ `request` already had, same names,
  `""` when absent (a second argument sets another default).
- ADR-0022: the dual C++/Python rule and the decisions the owner delegated.

### Changed
- A failing `done()` of an `async def` route (for example a bad header) now answers a 500 at once instead of leaving the client to the 504.
- **The worker pool wakes threads last-in first-out, and does not wake a second thread until a job has waited 500 us** (a scout
  thread watches the queue), so light `def` handlers run on one hot thread instead of passing the Python lock around. On two cores,
  `def` `hello` went from 16k to 36k req/s (3.13) and from 19k to 33k (3.14t); `item` from 12k to 25k and from 17k to 30k. A
  blocked handler delays queued requests by about 500 us. New `tests/cpp/test_worker_pool.cpp` (also run under ThreadSanitizer).

### Changed (async)
- **`async def` routes are about 30 percent faster:** one shared timer for `handler_timeout` instead of a timer per request, and a prebuilt JSON
  encoder (same output as `json.dumps`; CPython only, a circular structure now ends in a RecursionError, still a 500).
- **`async def` handlers start eagerly on Python 3.12+:** a handler that never waits finishes inside `dispatch` without a trip through the
  loop's ready queue (2 to 8 percent on `hello` / `item`).
- **The `async def` inbox is drained in batches:** one lock per batch of queued requests instead of one per request (about 12 percent on
  `hello`, 17 percent on `item`, 2 cores).
- **Faster answers for `str`, `dict` and `list` results,** and no `handler_timeout` bookkeeping for a handler that finishes at once
  (together with the eager start about 20 percent on `hello` / `item`, 2 cores).

### Fixed
- **`Server.listen()` could hang for ever after Ctrl+C on free-threaded Python (seen on the macOS 3.15t wheel test):** it waited on a
  `threading.Event`, and an interrupt that lands inside `Event.wait()` can leave that Event's internal lock taken (or released twice), after which
  `listen()` and the serving thread both block on it. It now polls a plain flag every 50 ms (`time.sleep` holds no lock). Two tests: Ctrl+C at
  many different moments, and a deterministic one that `listen()` never creates an `Event` or `Condition`.
- **The macOS wheel build works again:** `serve_directory` checked symlinks with `std::filesystem`, which needs macOS 10.15 while the
  wheels target 10.13. It now uses `realpath` on Linux and macOS (`std::filesystem` stays on Windows only). The tests also cover a sibling
  directory whose name starts like the mounted one, and a mounted directory given with a trailing slash.
- **`def` handlers were slow: a Python thread state was created and destroyed for every request.** Each pool thread now keeps
  its state (`httpp_py_pin_thread`). Measured on 2 cores, `oha`, 32 connections, plain `hello` route: CPython 3.13 9.4k -> 17.9k
  req/s, free-threaded 3.14.6t 1.9k -> 24.6k req/s. A CPU-bound `def` handler on 3.14t now scales with the cores
  (conc 1: 382, conc 8: 734 req/s) while 3.13 stays at about 330. A client that hangs up now closes a sync streaming generator
  at once (its `finally` runs), because on free-threaded CPython an object released from another thread is freed only when
  the thread that created it next runs Python.
- **A dependency without parameters failed on a route with path parameters** (`TypeError: dep() got an unexpected keyword argument 'n'`):
  every dependency was handed all the path parameters of the route, not only the ones it declares. Found by a stress test; it
  happened on every Python version.
- **405 instead of 404 for a wrong method.** A path that has a route for another method answers 405 with an `Allow` header
  (a GET route also allows HEAD). A path with no route at all is still 404. (Changed: code that checked for 404 on a wrong method.)
- **Static files no longer follow a symlink out of the mounted directory.** Such a link answers 404; a link that stays inside is served.
- **A path parameter that does not parse** (`n: int`, `x: float`) answers 422 with the FastAPI body
  `{"detail": [{"type": "int_parsing", "loc": ["path", "n"], ...}]}` instead of the old hand-made `{"error": ...}`.
- **abi3 wheel build.** `sha256_hex` took a typed memoryview, which needs `Py_buffer`; the stable ABI has it only from Python 3.11,
  and the cp39-abi3 wheel failed to compile (CI only). It now takes any bytes-like object without one (`bytes` in place, others copied
  once). `tests/python/test_abi3_safe.py` guards against typed memoryviews coming back.

### Added
- **Slowloris guard and a connection limit.** The whole request head (request line and headers) must now arrive within
  `header_timeout` seconds (default 10, `0` = no limit); before, a client sending one byte every few seconds kept the connection
  forever because only each single read had a timeout. `set_max_connections(n)` (C++) and `Server(max_connections=n)` refuse
  the connection over the limit with a 503 and close it (default: no limit). A slowly dripped request **body** is still only
  covered by `read_timeout` per read.
- **`HTTPException`, response classes, `httpp.status`.** `raise httpp.HTTPException(404)` (or `(status, detail, headers)`) in a
  handler or a dependency answers `{"detail": ...}` with that status; override it with `@app.exception_handler(HTTPException)`.
  `JSONResponse`, `PlainTextResponse`, `HTMLResponse`, `RedirectResponse` (307 by default), `StreamingResponse` and `FileResponse`
  can be returned from a handler and skip `response_model`. `httpp.status.HTTP_404_NOT_FOUND` and the rest. See ADR-0020.
- **gzip / deflate in the client, no system zlib.** Both the blocking and the async client send `Accept-Encoding: gzip, deflate`
  (unless you set the header yourself) and undo `Content-Encoding: gzip`, `x-gzip`, `deflate` (zlib, or raw as some servers send it)
  and chains of them, so `response.body` is always the real body. Headers are left as received, like requests and httpx. A
  coding we do not advertise (`br`, `zstd`) leaves the body untouched. `max_response_size` limits the **decoded** size
  (a zip bomb ends as `too_large`, status 413); corrupt data ends as `error_kind::other`, never as garbage. Set
  `Accept-Encoding: identity` to ask the server not to compress. `download()` is unchanged in what it sends but now saves a compressed response exactly as received, instead of failing with 415. Built on two vendored decoders/encoders, extracted unmodified:
  `src/third_party/stb_inflate` (the decoder of stb_image.h) and `src/third_party/stb_deflate` (the compressor of libstb), wrapped
  by the internal `httpp::detail::inflate` / `deflate` (zlib, gzip and raw; levels 0 to 9; Adler-32 and CRC-32 checked), and tested
  against Python's zlib and gzip in both directions. The compressor is ready for the server side (a GZip middleware) and for
  compressing request bodies; neither is wired yet.
- **Access log.** At the `info` log level the server writes one line per answered request (`GET /items/1 200`),
  including the answers the server makes itself (404, 413, ...). It goes through the library's own logging
  (AixLog), so `set_log_level`, `set_log_file` and `set_log_callback` apply to it. Nothing is built or written
  while the level is higher.
- **"Listening" line.** After a successful bind the server logs `listening on http://host:port` at `info`
  (also for `bind_to_any_port`, with the real port). A port that is already taken only produces the existing
  `cannot listen` error.

- **Python parity for downloads and hashing.** `Download.sha256()`, `Download.sha256_url()`, `Download.set_timeout()`,
  `Download.set_max_wait(seconds)` and `DownloadResult.sha256`; `httpp.utils.sha256_hex()`, `httpp.utils.sha256_file()` and
  `httpp.utils.parse_sha256_checksum()` (they raise `RuntimeError` where the C++ functions throw). `Client(keep_alive=False)`
  mirrors `httpp::client::set_keep_alive`.
- **Parity test.** `tests/python/test_cpp_python_parity.py` fails when a public C++ name has no Python counterpart and no documented reason.

### Changed
- **The C++ log goes through Python's `logging` automatically.** Lines arrive in `logging.getLogger("httpp")`, and that logger's level
  decides what C++ produces: `logging.getLogger("httpp").setLevel(logging.INFO)` (or `logging.config.dictConfig`) now also turns on the access
  log and the `listening on ...` line. `httpp.set_log_level(...)` sets the same logger level. `httpp run` still prints to stderr when the
  application configured no logging. An explicit `set_log_callback`/`set_log_file` replaces the bridge. See ADR-0018.
- `Client`/`AsyncClient` encode query strings and forms with the C++ `url::build_query` instead of `urllib` (identical output, about 4x
  faster for that step). One path-template parser in Python instead of two; one shared option block for `Client` and `AsyncClient`.
- **`httpp.run`, `app.run()` and `httpp run` log at `info` by default** (startup line and one line per request on
  stderr, like uvicorn). Use `log_level="warning"` or `--log-level warning` to keep only problems, `off` for
  nothing. `app.listen()` and the C++ API keep the default `warning`, so an embedded server stays quiet until you
  call `set_log_level`. Benchmarks should run at `warning`.
- **llhttp moved to `src/third_party/llhttp`** (it was `src/llhttp`). The files are unmodified: they match the checksums in
  `VENDORED.md` and the upstream `release/v9.4.3` tag byte for byte. Only the path changed.
- The `starting on ...` line printed by `run` before binding is gone; the `listening on ...` line replaces it and
  only appears once the port is really bound.
- **`httpp run` wins over a `listen` in the file.** While `httpp run module:app` imports the module, `app.listen()`
  and `app.run()` calls at import time (without an `if __name__ == "__main__":` guard) are skipped with a warning
  (`httpp: ignoring app.listen('127.0.0.1', 8001): ...`), so `--host`, `--port` and `--workers` apply. Before, the
  file's `listen` blocked the import, the CLI options were silently ignored, and `--workers` failed with a bind
  error. Running the file with `python file.py` is unchanged. Only a string target (`"module:app"`) is affected.

### Measured
First WebSocket and Drogon numbers, from the full `bench` target on a 16 thread machine (medians of 3 rounds of 5 s, 64 connections, `powersave` governor, one box,
so compare rows of this run only):
- WebSocket echo, 64 byte messages: Python 3.14 httpp 287k msg/s (1 I/O thread) against 122k for uvicorn and 111k for the `websockets` library; 3.14t with
  4 I/O threads 715k msg/s. C++: httpp 339k, 648k, 1.05M and 973k msg/s with 1, 2, 4 and 8 I/O threads against 373k, 755k, 1.06M and 974k for Drogon.
- `hello` on Python 3.14: `def` 241k req/s with 1 I/O thread and 521k with 4, against 78k for uvicorn and 22k for FastAPI with uvicorn (run after the engine changes).
- 0 errors in every row (3 rounds each), which is the first full run without a lost server since the `Date` cache fix.

### Known limitations
- WebSocket: no `permessage-deflate`, and no TLS of its own (`wss` needs a TLS proxy in front; not tried). On WebSocket routes there are no dependencies,
  no injected `response`, no background tasks, no OpenAPI entry and no `APIRouter.websocket` yet. The engine closes with a lingering close (up to 2 s), so the final
  close code can arrive after the close frames.
- Memory per idle WebSocket connection is higher than in Drogon and in the `websockets` library: about 20 KiB (C++) and 24 KiB (Python) against 8.5 KiB and
  12 KiB (10,000 connections, one run).
- Plain HTTP in C++ is still slower than Drogon at the same number of I/O threads, by 7 to 17 percent after the changes above (`hello`, `route_io`: 256k, 460k and 630k
  req/s with 1, 2 and 4 threads against 281k, 496k and 760k; a later run than the one in "Measured", same machine). WebSocket echo is about 9 to 14 percent behind with 1 or 2 threads and level with 4 or more.
- A free-threaded Python still defaults to one I/O thread (`io_threads`), which is about 2.4 times slower than 4 on the `hello` benchmark; set it by hand.
- The WebSocket code was run on Linux; Windows and macOS were covered by CI only. ThreadSanitizer is clean on the whole C++ suite (384 test cases); the Python layer was not run under it.

## [0.7.0] - 2026-10-17

The biggest release so far: a native async client and server, a FastAPI-style Python layer with OpenAPI and Swagger
UI, and a download that verifies, locks and times out.

### Added

#### Client
- **Async client** (`Client.run_async()`, `AsyncClient`) running on the library's own non-blocking network thread
  (Asio) with the vendored `llhttp` parser, instead of a shared thread pool. Keep-alive connection pooling and reuse
  across requests, redirect handling, TLS and proxy support.
- **Streaming responses** and **structured errors** (`httpp.errors`, with an error kind such as `timeout`) for both
  the blocking and the async client; extra URL helpers in `httpp/url.hpp`.

#### Server (C++ and Python)
- **Async HTTP server** (`async_server`, with a worker pool). It speaks plain HTTP: the server has no TLS yet. TLS (Mbed TLS)
  is in the async client, with session caching: TLS contexts for identical (verify, CA file) settings are built once and reused.
- **Python handlers:** route handlers with path parameters, `async def` handlers, and `listen()` that blocks and
  returns a status (0.6.1); new in this release: `ResponseHead`, `file()`, `redirect()`, `set_cookie()`,
  `ServerRequest.cookies`, streaming responses from generators and async generators, `@app.before_request`,
  `@app.after_request`, error and exception handlers, startup and shutdown hooks, and `handler_timeout` (60 s by
  default) that cancels a hanging async route and answers 504. Header values are checked against injection.
- **OpenAPI 3.1 and Swagger UI for Python.** Schema generation with Pydantic v2 models, per-route options (`summary`,
  `tags`, `response_model`, ...), and `Query`, `Path`, `Header`, `Cookie`, `Body` and `Depends` (dependencies with their
  own scopes and caching) that drive both the request handling and the generated document, so the docs cannot drift
  from the behaviour.
- **`httpp::mount_swagger_ui(server, docs_path = "/docs", openapi_url = "/openapi.json", title = "API")`** in C++:
  serves the page and the Swagger UI files (`swagger-ui-bundle.js`, `swagger-ui.css`, `swagger-api-icon.svg`) from
  bytes compiled into the library, with `Cache-Control: public, max-age=86400`. Title and URL are escaped; an invalid
  path throws `std::invalid_argument`. The Python server's `/docs` is now served from C++, without the GIL, and there
  is no CDN or data directory involved.
- **`cmake/EmbedFile.cmake`** (`embed_files()`): compiles data files into a header at configure time (`constexpr`
  data in `.rodata`, binary-safe, 32 bytes per source line).

#### Downloads
- **`download::sha256(hex)` and `download::sha256_url(url = "")`**: verify the finished file against a SHA-256 digest
  (any letter case) or against a checksum file in `sha256sum` format (default `<url>.sha256`). The checksum file is
  fetched first, so a missing or malformed one (an HTML error page, a name for another file, several lines) fails the
  call before any download. A mismatch deletes the file, returns `ok = false` and says what the digest actually was.
  With `resume()`, a complete-looking but corrupt file is downloaded again once from byte 0. `download_result` has a
  new `sha256` field.
- **`httpp/sha256.hpp`**: `sha256_hex()`, `sha256_file()` (chunked) and `parse_sha256_checksum()`, on top of the
  Mbed TLS already linked for HTTPS. About 165 MB/s on the machine it was measured on (130 MB file in 0.8 s).
- **`download::set_timeout(seconds)`**: connect, read and write timeouts of the transfer (like
  `Client::set_timeout`), and also how long to wait for another process that holds the same file's lock.
- **`download::set_max_wait(milliseconds)`**: how long a lock that is *not being updated* is waited for before its
  owner is taken for dead (default 2 s; the wait is `min(max_wait, timeout)`).
- **Cross-process lock, always on.** A download takes the directory `<dest>.lock` and its owner rewrites
  `<dest>.lock/alive` every 200 ms at most; a lock that keeps being updated is waited for as long as it lives, one that
  stops is taken over after `min(max_wait, timeout)`. A waiter that finds the file finished and matching `sha256()` /
  `sha256_url()` uses it without a request (`skipped = true`). If the lock directory cannot be created the download
  goes ahead unlocked. Written with `mkdir`/`rmdir` and plain files, no `<filesystem>`.

#### Library and tooling
- **Logging** through the vendored AixLog, with a `set_log_*` API in C, C++ and Python.
- **Python wheels:** one `cp39-abi3` wheel per platform for every GIL-enabled Python from 3.9 up, plus version-specific
  free-threaded wheels (`cp313t`, `cp314t`, `cp315t`), because free-threaded builds cannot load abi3 wheels.
- **Documentation:** `docs/Architecture.md` (design, goals and test strategy), `docs/agent.md`, Python micro-benchmark
  for the validators, and README sections for streaming, files, hooks, error handling and the client's new
  capabilities.

### Changed
- **Mbed TLS 4.2.0** (release tag), with threading support switched on (`MBEDTLS_THREADING_C`, via
  `cmake/httpp_mbedtls_config.h`): pthread on POSIX, a small `SRWLOCK`/`CONDITION_VARIABLE` layer on Windows, or on
  every platform with `-DHTTPP_MBEDTLS_THREADING_ALT=ON`. `MBEDTLS_SELF_TEST` is off.
- **`HTTPP_EMBED_AS_ARRAY` is `ON` on every platform** (it was MSVC only). One code path everywhere and no
  `-Woverlength-strings` warning for the 1.5 MB Swagger UI bundle. Cost: compiling that translation unit takes about
  4.6 s instead of 0.9 s with GCC, once per build. `-DHTTPP_EMBED_AS_ARRAY=OFF` gives string literals again.
- The Swagger UI tab icon is a real SVG file served at `/docs/swagger-api-icon.svg`; the PNG favicon is gone.
- `<filesystem>` is not used any more (macOS 10.13 compatibility); `download.cpp` and the lock use `std::ifstream`,
  `std::remove` and platform calls.
- Asio now drives both server loops and the async client (documentation and build comments updated).
- Build requirement: `scikit-build-core[pyproject]>=1.1`; the version is read through the standard
  `[[tool.dynamic-metadata]]` table instead of the deprecated `tool.scikit-build.metadata`.
- A download creates (and removes again) `<dest>.lock` next to the destination file; the destination's directory must
  be writable for the lock to work (otherwise the download is not locked).

### Fixed
- Concurrent TLS handshakes shared Mbed TLS's global random generator without a lock; two handshakes could draw the
  same random bytes. Found with ThreadSanitizer, fixed by the threading support above.
- Async `after`/`error` hooks, or a slow client, could block the event loop and deadlock concurrent requests; the
  Python adapter now dispatches synchronous work directly when no async hook is registered.
- **Windows:** a finished download did not replace an existing file (`std::rename` fails when the target exists); it is
  now an atomic `MoveFileExA(MOVEFILE_REPLACE_EXISTING)`. `ws2_32` is linked for `send()`/`WSAGetLastError()`, and the
  `EmbedFile` generator no longer emits a zero-size array for an empty file (MSVC `C2466`).
- **macOS:** link the Keychain frameworks for the C++ tests; a URL-encoding test relied on `std::isalnum` being false
  for non-ASCII bytes.
- Coverage builds use atomic profile counters (the server is multi-threaded, lcov saw negative counts); free-threaded
  wheel builds prefer binary wheels so `rpds-py` is not built from source.
- Tests for concurrent async downloads measure event-loop ticks and overlapping streams instead of wall-clock time.
- **Release pipeline:** wheels are uploaded as one flat artifact, so `dist/` holds only `.whl` and `.tar.gz` files and
  the PyPI upload no longer fails on a `wheelhouse` sub-directory. Windows wheels are not run through `delvewheel`
  (`httpp_core.dll` is loaded from `httpp/lib/` at import time), and the PyPI step uses `skip-existing`.

### Known limitations
- The async client has no timeout yet, and async hooks hold a thread while they run.
- `download::set_timeout`, `set_max_wait`, `sha256` and `sha256_url` are not exposed in the Python binding yet.
- A lock whose owner stops updating it for more than `max_wait` (heavy load, a suspended process) is taken over, and two
  downloads can then run at once. Two waiters taking over the same dead lock in the same instant, or writing the same
  destination, can write it together; use `sha256()` so a damaged result is caught and deleted. Keep `max_wait`
  at 1 s or more.
- The Windows and macOS changes of this release were checked by CI only; the lock was not run under ThreadSanitizer.
- `/docs` has been looked at in Chromium only.

## [0.6.1] - 2026-10-03

### Added
- Python route handlers with path parameters and `async def` handlers; native async support for Python and C++
  handlers.
- `listen()` blocks and returns a status in the C++ and Python bindings.
- Runnable standalone Python examples: server basics, routing, request and response handling, async handlers, static
  files, error handling.

### Changed
- Asio 1.34.2 is a git submodule (no system dependency, better scalability).
- README rewritten with a clearer description and structure; the Asio documentation was consolidated.

### Fixed
- Windows: winsock type redefinition and missing socket headers in the tests; a Ctrl+C listener test that cannot run
  there (Popen) is skipped and the SIGINT test was replaced with an `interrupt_main` variant.

## [0.6.0] - 2026-09-16

### Added
- Resumable downloads with skip-if-complete.

### Changed
- **Breaking:** `download_file` was renamed to `download` and folded into one fluent builder.

## [0.5.1] - 2026-09-16

### Added
- `max_response_size` for the client and `follow_redirects` for downloads.
- `abi3` wheels in CI.

### Changed
- `std::filesystem::remove` replaced by `std::remove` in downloads.

## [0.5.0] - 2026-09-15

### Added
- HTTPS support through Mbed TLS (documented in the API docs).

### Changed
- Mbed TLS sources are generated before CMake configures; `jinja2` is a build dependency; submodules are no longer
  fetched recursively by default.
- macOS: the Keychain frameworks are linked into `httpp_core`.
- Vendored-code warnings are suppressed and class-level exports avoided.
- Linux wheels are repaired with `auditwheel`.

## [0.4.0] - 2026-09-14

### Fixed
- Linux wheels are repaired with `auditwheel` in cibuildwheel builds.

## [0.3.0] - 2026-09-14

Release only; no functional change recorded in the history.

## [0.2.0] - 2026-09-13

### Added
- curl-compatible API and CLI commands.

## [0.1.1] - 2026-09-13

### Fixed
- Linux wheels are repaired with `auditwheel` instead of copying libraries.

## [0.1.0] - 2026-09-12

First release.

### Added
- C++17 core: client, server and URL parsing (`liburlparser`, IPv4, IPv6 and Public Suffix List aware).
- Downloads with a progress bar (vendored `indicators`).
- Python package with `httpp_core` as a shared library bundled in the wheel; macOS rpath handling.
- CMake package: consumers find httpp through `httpp.get_cmake_dir()`; `HTTPP_BUILDING_DLL` for the shared core.
- Single umbrella header `include/httpp.h` that carries the version, and `version.py` for tagging and releases.
- CMake and wheels workflows in CI.

[Unreleased]: https://github.com/MohammadRaziei/httpp/compare/v0.9.0...HEAD
[0.9.0]: https://github.com/MohammadRaziei/httpp/compare/v0.8.1...v0.9.0
[0.8.1]: https://github.com/MohammadRaziei/httpp/compare/v0.8.0...v0.8.1
[0.8.0]: https://github.com/MohammadRaziei/httpp/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/MohammadRaziei/httpp/compare/v0.6.1...v0.7.0
[0.6.1]: https://github.com/MohammadRaziei/httpp/compare/v0.6.0...v0.6.1
[0.6.0]: https://github.com/MohammadRaziei/httpp/compare/v0.5.1...v0.6.0
[0.5.1]: https://github.com/MohammadRaziei/httpp/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/MohammadRaziei/httpp/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/MohammadRaziei/httpp/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/MohammadRaziei/httpp/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/MohammadRaziei/httpp/compare/v0.1.1...v0.2.0
[0.1.1]: https://github.com/MohammadRaziei/httpp/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/MohammadRaziei/httpp/releases/tag/v0.1.0