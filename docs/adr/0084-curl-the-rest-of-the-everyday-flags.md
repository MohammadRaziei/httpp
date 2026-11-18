# ADR-0084: `httpp curl` gets the rest of the everyday flags and several URLs

- **Status:** accepted
- **Decided by:** the project owner ("close the missing flags, `-s -S -w -O -T -D -d @file --compressed --retry` and several URLs, and the documentation")
- **Source:** `src/cli/httpp_cli.cpp`, `tests/python/test_cli.py`, `docs/site/docs/client/curl-command.mdx`, the examples of `docs/site/docs/server/*.mdx`
- **Follows:** ADR-0077, ADR-0079, ADR-0083

## Context

After ADR-0083 `httpp curl` behaved like curl on errors, but a script still could not stay quiet, name its output file, show only the status code, upload a file, keep the headers, read a body from a file, retry, or fetch more than one URL. The documentation used the real `curl` in its examples, and could not use `httpp curl` because it printed no head.

## Decision

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
- **The body is streamed.** `httpp::request` has `on_body(fn(std::string_view) -> bool)` and `on_head(fn(const response&) -> bool)`, built on the engine's `body_sink` (ADR-0052) and not a new class. With `on_body` the final body goes to the callback as it arrives, `response::body` stays empty, `max_response_size` does not apply, a `false` stops the transfer (`error_kind::canceled`), and a `false` from `on_head` skips the body but still returns the response. A followed redirect is never offered to them. The request asks for `Accept-Encoding: identity` (the engine delivers streamed bytes as sent and has no incremental decoder); a header of the caller wins. The callbacks run on the second engine (`download_async_http()`), so a blocking write never stalls plain requests.
- `httpp curl` uses it: stdout or the `-o`/`-O` file is written piece by piece and flushed, so `httpp curl parrot.live` shows the animation and a big answer never sits in memory. The file is opened when the final head is accepted (an error status with `-f`, a refused connection, or an answer that will be retried leaves no file). `-i`, `-I`, `-D` and `-v` print the head before the first piece. A failed write, including a closed pipe (`httpp curl ... | head`), stops the transfer with exit code 23, as curl does: the command ignores SIGPIPE for that. A transient status that will be retried is skipped and not printed. `%{size_download}` counts the body bytes written. `--compressed` stays accepted and still changes nothing visible: the answer arrives uncoded.
- Rejected: reading the engine's internal `body_sink` from the CLI (a shortcut around the public API, which has to stay dual in C++ and Python); a new streaming class (the settings of `request` would be repeated in it); a streaming gzip decoder (not needed for curl, which writes the bytes it gets).
- The documentation gets a page, *The httpp curl command*, and the server pages use `httpp curl`. Their `# HTTP/1.1 201 Created` comments lose the reason phrase, because `httpp curl -i` prints `HTTP/1.1 201`.

## Consequences

- Not done: the Python binding of `on_body` and `on_head` (`test_cpp_python_parity.py` lists both in `NOT_EXPOSED` as temporary; remove those lines when it is bound). Decide there how a Python callback gets the GIL on the streaming engine's thread, and whether an iterator form (`for piece in request.stream()`) is the Python spelling.

- `-o` changed from one value to a list; one `-o` with one URL works as before.
- Still missing: `-F`, `--json`, `-G`, `-b`, `-x`, `-k`, `--cacert`, `--data-binary`, `-C`, `-r`, `--http2`. `httpp::request` has proxies, cookies, TLS options, JSON and form bodies; the command does not expose them yet. `-m` still limits each step, not the whole transfer. The Python binding of `request::on_body` and `on_head` is not done yet (see the next section).

## Related

ADR-0077, ADR-0079, ADR-0083
