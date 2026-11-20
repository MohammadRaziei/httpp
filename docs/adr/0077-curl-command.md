# ADR-0077: `httpp curl`: a curl-compatible command that only calls `httpp::request`

- **Status:** accepted (merged ADR; each part keeps its own status line)
- **Merged on:** 2026-11-20, at the request of the project owner (the ADRs of one subject were spread over many numbers). The text of every part is kept; the old numbers still name the parts below.
- **Merges:** ADR-0077, ADR-0083, ADR-0084, ADR-0098

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history, in the order they were decided.

- **The command is a thin face on the library.** Where `httpp::request` lacks something an everyday request needs, it is added to `httpp::request` (and so to Python's `Request`), never to a second class or to the CLI (0077, 0098; the same rule continues in ADR-0097, 0099 and 0102 for elapsed time, retry and saving to a file).
- **Retired:** `httpp/curl.hpp` and `httpp::curl::request` (0077), `httpp/curl_compat.h` and every `curl_easy_*` name (0083). Use `httpp::request` (Python `httpp.Request`).
- **Errors behave like curl** (0083): any HTTP status exits 0 and prints its body; `-f` turns 400 and above into exit 22; a transfer with no answer exits with curl's code (3 invalid url, 7 connection, 28 timeout, 35 tls, 47 too many redirects, 63 too large, 1 otherwise).
- **Output is byte for byte**, with no added newline after the body (0077).
- **Flags were added in four steps:** `-i -I -v -L -m -u` (0077); the errors above, `-f`, repeated `-d` (0083); `-s -S -w -O -T -D -d @file --compressed --retry`, several URLs, proxies and the curl version in the User-Agent (0084); `-k --cacert -J --output-dir --create-dirs --json --fail-with-body` (0098). The parts hold the exact semantics.
- Where an early part says a flag is "still missing", the later parts of this ADR are the update (for example 0077's "non-2xx exits 1" is replaced by 0083).

## Parts

### ADR-0077: `httpp curl` gets the everyday flags, and `httpp/curl.hpp` is retired

- **Status:** accepted
- **Decided by:** the project owner ("add the cheap flags first", "enrich `httpp::request`, not `httpp::curl::request`", "retire `httpp::curl::request` and all of `include/httpp/curl.hpp`", "our API is very good as it is")
- **Source:** `src/cli/httpp_cli.cpp`, `tests/python/test_cli.py`, `tests/python/test_cpp_python_parity.py`; removed: `include/httpp/curl.hpp`, `src/core/curl.cpp`, `tests/cpp/test_curl.cpp`

#### Context

`httpp::curl::request` was a curl-flavoured builder with six methods (`method`, `header`, `data`, `content_type`, `timeout`, `follow_redirects`), a thin face on `httpp::request` (ADR-0053). `httpp::request` has all of them and more (auth, cookies, proxy, TLS options, JSON and form bodies, async). Nothing used the curl class: the `httpp curl` command already built an `httpp::request`, and the parity test only mapped it to `httpp.Request`. It was a second, weaker name for the same thing.

The `httpp curl` command took only `-X`, `-H`, `-d`, `-o`. It could not show the status or the headers of an answer, could not follow a redirect or set a timeout although `httpp::request` can, and it added a `\n` after every body, which corrupts binary output.

#### Decision

- `httpp curl` gets `-i/--include` (status line and headers before the body), `-I/--head` (send `HEAD`, print the head only), `-v/--verbose` (the request line and the `-H` headers, then the response status and headers, on stderr), `-L/--location`, `-m/--max-time SECONDS` (a whole number, at least 1) and `-u/--user USER:PASSWORD` (basic authentication; only the first colon splits, a missing colon is an error).
- The head is printed as `HTTP/1.1 <status>` followed by the headers, with CRLF line ends. The engine speaks HTTP/1.1 and the response carries neither a version nor a reason phrase, so none is invented. `-v` prints only the headers given with `-H`, not the ones the engine adds itself (such as `Host`).
- `httpp curl` and `httpp download` (to stdout) no longer add a newline after the body, and stdout is switched to binary mode on Windows.
- `httpp/curl.hpp`, `httpp::curl::request`, `curl.cpp` and `test_curl.cpp` are removed. There is no replacement: use `httpp::request`. The libcurl-style C API in `httpp/curl_compat.h` is a different thing and stays.
- Where `httpp::request` is missing something an everyday request needs, it is added to `httpp::request` (and so to Python's `Request`), never to a second class.

#### Consequences

- **Breaking for C++ code (major is 0):** `#include "httpp/curl.hpp"` and `httpp::curl::request` no longer exist; the same calls work on `httpp::request`.
- The parity test lost its `("curl.hpp", "request")` entry.
- Scripts that relied on the extra trailing newline of `httpp curl` or `httpp download` see the body byte for byte now.
- Not changed here and still different from curl: a non-2xx answer exits with 1 (curl needs `-f` for that), a transport error says only `status 0`, `-d` takes one value. These are the next step.

#### Related

ADR-0041, ADR-0051, ADR-0053, ADR-0062, ADR-0067

### ADR-0083: `httpp curl` behaves like curl on errors, and `httpp/curl_compat.h` is retired

- **Status:** accepted
- **Decided by:** the project owner ("those gaps must be fixed", "retire `curl_compat` too", "keep presenting httpp as the modern replacement for curl, as for aiohttp, httpx and requests")
- **Source:** `src/cli/httpp_cli.cpp`, `tests/python/test_cli.py`; removed: `include/httpp/curl_compat.h`, `src/core/curl_compat.cpp`, `tests/cpp/test_curl_compat.cpp`; edited: `include/httpp.h`, `README.md`, `tests/cmake/consumer_project/main.cpp`
- **Follows:** ADR-0077 (its sentence that `curl_compat.h` stays is replaced by this ADR)

#### Context

After ADR-0077 `httpp curl` could show heads and follow redirects, but it still differed from curl where scripts notice it: every non-2xx answer exited with 1, a failed transfer said only `status 0` and exited 1 whatever the cause, it left an empty `-o` file behind, and only one `-d` was accepted.

`httpp/curl_compat.h` was a 15-option `curl_easy_*` subset with its own error numbers. Nothing in the project used it besides its test and the installed-package smoke test, and it could not deliver "change the header, keep the code" (no auth, proxy, TLS options, cookies, `multi`), so it promised more than it kept and had to follow every change of `httpp::request`.

#### Decision

- An HTTP answer is not a failure: any status exits 0 and prints its body. `-f/--fail` turns a status of 400 or more into exit 22 with no output and no `-o` file (3xx is not an error, as in curl).
- A transfer with no HTTP answer prints `error: <what happened>` (the reason from `response::error_message` when it adds something) and exits with curl's code: 3 `invalid_url`, 7 `connection`, 28 `timeout`, 35 `tls`, 47 `too_many_redirects`, 63 `too_large`, 1 for the rest. No `-o` file is written. `connection` covers a failed lookup too, because the engine does not report it separately.
- Several `-d` are joined with `&`; the URL may stand before or after the options.
- `httpp/curl_compat.h`, `curl_compat.cpp` and their test are removed without a replacement. Code that used `curl_easy_*` moves to `httpp::request` (Python: `httpp.Request`). The README presents `httpp curl` and `httpp::request` as a modern counterpart of curl, in the same way httpp is presented next to aiohttp, httpx and requests, and says plainly which curl flags are missing.

#### Consequences

- **Behaviour change:** a script that relied on `httpp curl` failing for 404 must pass `-f`. Scripts that tested for exit code 1 on a refused connection now see 7.
- **Breaking for C++ code (major is 0):** `#include "httpp/curl_compat.h"` and every `curl_easy_*`, `CURLOPT_*`, `CURLINFO_*`, `CURLE_*` name are gone.
- `httpp download` is unchanged (it still exits 1 on a non-2xx answer): it is a download command, not curl.
- Still not in `httpp curl`: `-s`, `-S`, `-w`, `-A`, `-e`, `-O`, `-T`, `-D`, `-d @file`, `--compressed`, `--retry`, `--connect-timeout`, several URLs. `httpp::request` is to be enriched first (decided direction, the first additions are not chosen yet).

#### Related

ADR-0041, ADR-0053, ADR-0062, ADR-0067, ADR-0077

### ADR-0084: `httpp curl` gets the rest of the everyday flags and several URLs

- **Status:** accepted
- **Decided by:** the project owner ("close the missing flags, `-s -S -w -O -T -D -d @file --compressed --retry` and several URLs, and the documentation")
- **Source:** `src/cli/httpp_cli.cpp`, `tests/python/test_cli.py`, `docs/site/docs/client/curl-command.mdx`, the examples of `docs/site/docs/server/*.mdx`
- **Follows:** ADR-0077, ADR-0079, ADR-0083

#### Context

After ADR-0083 `httpp curl` behaved like curl on errors, but a script still could not stay quiet, name its output file, show only the status code, upload a file, keep the headers, read a body from a file, retry, or fetch more than one URL. The documentation used the real `curl` in its examples, and could not use `httpp curl` because it printed no head.

#### Decision

- `-s/--silent` hides the `error:` line and the retry warning; `-S/--show-error` brings the error line back. The exit code never changes.
- `-w/--write-out FORMAT` prints FORMAT after each transfer (also after a failed one) with `%{http_code}` (`000` without an answer), `%{content_type}`, `%{size_download}` (the body after decoding), `%{exitcode}`, `%{errormsg}`, `%{url}`, and `\n \r \t \\ %%`. A `%{name}` that is not one of these stays as typed. `@file` reads the format from a file.
- `-O/--remote-name` names the file after the last piece of the URL path, for URLs that have no `-o`. No usable name (`/`, `.`, `..`, a backslash) is exit code 23.
- `-T/--upload-file FILE` sends the file with `PUT` (`-` is stdin) and appends the file's name to a URL that ends with `/`. With `-d` it is an error.
- `-D/--dump-header FILE` writes the head of every answer to the file (`-` is stdout). The head is the one of the final answer: the engine does not keep the heads of redirects.
- `-d @file` and `-d @-` read the body from a file or stdin and drop `\r` and `\n`, as curl does for `-d`. `--data-binary` is not added. An unreadable file is exit code 26.
- `--compressed` is accepted and does nothing. When this ADR was written the client always sent `Accept-Encoding: gzip, deflate` and decoded the answer; since the body is streamed (below) the command asks for `identity` instead (an explicit `-H` still wins).
- `--retry N`, `--retry-delay SECONDS` and `--retry-connrefused`: another try after a timeout or a status of 408, 429, 500, 502, 503 or 504 (a failed connection only with `--retry-connrefused`). The wait is 1, 2, 4, ... seconds up to 600, or the given delay. `Retry-After` is not read.
- Several URLs are fetched in turn. `-o` is matched with the URLs by position; every other option applies to all of them. A failure does not stop the rest and the exit code is the one of the first failure. (This choice was made here and was not checked against what curl returns.)
- A URL with no `://` is read as `http://` (curl does the same), and a URL with a space or a control character is exit code 3 before anything is sent (curl: "URL rejected"). The check is in the command, not in `httpp::url`: the URL parser still accepts a space in a path, which a library caller meets as `invalid request line` from the server. ponytail: moving the check into `url_is_http` is the better home and is not done here.
- Default `User-Agent` of `httpp curl` is `curl/8.18.0`, the curl release whose behavior the command follows (`k_curl_compat_version`); `-A` and `-H` win. `httpp curl --version` and `-V` print `httpp curl <version> compatible with curl <curl version>`. `httpp::request` keeps its own User-Agent: only the command presents itself as curl, because it is meant to replace it in scripts (parrot.live, for one, streams only to `curl/...` and redirects every other agent).
- A missing URL is reported as curl does and exits with 2: bare `httpp curl` prints `httpp curl: try 'httpp curl --help' for more information`, and with other options it first prints `httpp curl: no URL specified`. The `url` argument is not `required()` in CLI11 for this reason (its own message was `url is required`).
- **Proxies.** `-x`/`--proxy URL` and `--noproxy LIST` are added, and without `-x` the proxy comes from the environment (`http_proxy`, `https_proxy`, `all_proxy`, `no_proxy`), all through `httpp::resolve_proxy` (ADR-0067). `-x ""` turns the proxy off even when the environment sets one, `--noproxy` replaces `no_proxy` and also applies to `-x`. An unusable proxy is exit code 5 (curl's "could not resolve proxy"). `-v` prints `* Uses proxy scheme://host:port` (never the login). Found when `httpp curl parrot.live` hung on a machine where `http_proxy` was set: curl went through the proxy and httpp connected directly. ponytail: the proxy is chosen for the first URL; a redirect to another host keeps it.
- **The body is streamed.** `httpp::request` has `on_body(fn(std::string_view) -> bool)` and `on_head(fn(const response&) -> bool)`, built on the engine's `body_sink` (ADR-0052) and not a new class. With `on_body` the final body goes to the callback as it arrives, `response::body` stays empty, `max_response_size` does not apply, a `false` stops the transfer (`error_kind::canceled`), and a `false` from `on_head` skips the body but still returns the response. A followed redirect is never offered to them. The request asks for `Accept-Encoding: identity` (the engine delivers streamed bytes as sent and has no incremental decoder); a header of the caller wins. The callbacks run on the second engine (`download_async_http()`), so a blocking write never stalls plain requests.
- `httpp curl` uses it: stdout or the `-o`/`-O` file is written piece by piece and flushed, so `httpp curl parrot.live` shows the animation and a big answer never sits in memory. The file is opened when the final head is accepted (an error status with `-f`, a refused connection, or an answer that will be retried leaves no file). `-i`, `-I`, `-D` and `-v` print the head before the first piece. A failed write, including a closed pipe (`httpp curl ... | head`), stops the transfer with exit code 23, as curl does: the command ignores SIGPIPE for that. A transient status that will be retried is skipped and not printed. `%{size_download}` counts the body bytes written. `--compressed` stays accepted and still changes nothing visible: the answer arrives uncoded.
- **`-m` limits the whole transfer.** `httpp::request::max_time(seconds)` (Python `Request.max_time`, `httpp.request(..., max_time=N)`) is a new limit for the whole request, redirects and a streamed body included: the first hop sets one deadline (`request_spec::deadline`) that every redirect hop inherits, and each exchange in the engine arms one `steady_timer` for it. When it runs out the exchange fails with `error_kind::timeout` even while bytes still arrive. `timeout()` keeps its meaning (each step, default 300 s) and is not changed, because turning it into a total would cut off every large download that uses it today. `httpp curl -m` now calls `max_time`, so `httpp curl -m 10 parrot.live` ends after ten seconds with exit code 28. `0` is no limit, a negative number is `std::invalid_argument` / `ValueError`. ponytail: `client` and `async_client` have no `max_time` setter yet (they keep `timeout`), the same gap as `max_redirects` in ADR-0079.
- Rejected: reading the engine's internal `body_sink` from the CLI (a shortcut around the public API, which has to stay dual in C++ and Python); a new streaming class (the settings of `request` would be repeated in it); a streaming gzip decoder (not needed for curl, which writes the bytes it gets).
- The documentation gets a page, *The httpp curl command*, and the server pages use `httpp curl`. Their `# HTTP/1.1 201 Created` comments lose the reason phrase, because `httpp curl -i` prints `HTTP/1.1 201`.

#### Consequences

- Not done: the Python binding of `on_body` and `on_head` (`test_cpp_python_parity.py` lists both in `NOT_EXPOSED` as temporary; remove those lines when it is bound). Decide there how a Python callback gets the GIL on the streaming engine's thread, and whether an iterator form (`for piece in request.stream()`) is the Python spelling.

- `-o` changed from one value to a list; one `-o` with one URL works as before.
- Still missing: `-F`, `--json`, `-G`, `-b`, `-x`, `-k`, `--cacert`, `--data-binary`, `-C`, `-r`, `--http2`. `httpp::request` has proxies, cookies, TLS options, JSON and form bodies; the command does not expose them yet. The Python binding of `request::on_body` and `on_head` is not done yet (see the next section).

#### Related

ADR-0077, ADR-0079, ADR-0083

### ADR-0098: `httpp curl` gets `-k`, `--cacert`, `-J`, `--output-dir`, `--create-dirs`, `--json` and `--fail-with-body`; the pieces live in the library

- **Status:** accepted
- **Decided by:** the project owner ("implement the high-priority ones", and "add the capabilities to `httpp::request` as far as possible, not to the CLI")
- **Source:** `include/httpp/path.hpp`, `include/httpp/url.hpp`, `include/httpp/client.hpp`, `src/cli/httpp_cli.cpp`, `docs/site/docs/client/curl-command.mdx`
- **Follows:** ADR-0084, ADR-0097

#### Context

`httpp curl` already had `-X`, `-x`/`--noproxy` and the proxy variables (ADR-0097). What was left of the common curl flags was reaching a server with a certificate of your own, naming and placing the downloaded file, sending JSON, and getting the error body of a failed request.

#### Decision

The CLI is kept to flags and exit codes; what a flag does is a library capability where there is one:

- **`-k` / `--insecure`** is `request::verify(false)` and **`--cacert FILE`** is `request::ca_file(path)`: both existed. `-k` wins over `--cacert`. A `--cacert` file that is not a file is exit code 77 (CURLE_SSL_CACERT_BADFILE) before anything is sent. An untrusted certificate stays exit code 35.
- **`--json DATA`** sends the pieces joined with no separator (`@file` is read as it is, line breaks kept: curl treats `--json` like `--data-binary`), and adds `Content-Type` and `Accept` `application/json` unless `-H` gives them. It is refused together with `-d` or `-T` (exit code 1).
- **New in the library**, used by the CLI and open to everyone:
  - `httpp::path::mkdirs()`: pathlib's `mkdir(parents=True, exist_ok=True)`; true when the folder exists afterwards.
  - `httpp::url_filename(url)`: the last piece of the URL path (the name `-O` uses), "" when there is none; this replaces the CLI's own copy.
  - `httpp::response::filename()`: the name in `Content-Disposition`, as a bare name (folder part cut off, control characters dropped, "" for nothing usable). `filename*=` is not read.
- **`-J` / `--remote-header-name`** (with `-O` only; without `-O` it does nothing, as in curl) names the file `response::filename()`, falling back to the URL's name. It never replaces a file that exists: exit code 23 with "Refusing to overwrite", as curl does.
- **`--output-dir DIR`** puts the file of `-o` and `-O` in DIR (`DIR / name`; an absolute name stays where it is); **`--create-dirs`** makes the missing folders above it with `path::mkdirs()` when the answer is accepted, so a transfer that never happens leaves no folder behind. A missing folder without it is exit code 23.
- **`--fail-with-body`** is `-f` that keeps the body: exit code 22 for 400 and up, the body is printed or saved. The two exclude each other (usage error, exit code 2).

#### Consequences

- No new `request` method was needed; the transfer features of `request` this CLI uses are all the ones it had.
- The three library helpers are inline in their headers, so the C++/Python parity test does not list them. `Response.filename` is not in Python yet.
- `-v` still shows only the headers `httpp curl` sends, not the engine's own.

#### Related

ADR-0084, ADR-0097
