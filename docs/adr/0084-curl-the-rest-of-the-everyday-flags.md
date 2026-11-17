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
- `--compressed` is accepted and does nothing: the client always sends `Accept-Encoding: gzip, deflate` and decodes the answer (an explicit `-H` still wins).
- `--retry N`, `--retry-delay SECONDS` and `--retry-connrefused`: another try after a timeout or a status of 408, 429, 500, 502, 503 or 504 (a failed connection only with `--retry-connrefused`). The wait is 1, 2, 4, ... seconds up to 600, or the given delay. `Retry-After` is not read.
- Several URLs are fetched in turn. `-o` is matched with the URLs by position; every other option applies to all of them. A failure does not stop the rest and the exit code is the one of the first failure. (This choice was made here and was not checked against what curl returns.)
- A URL with no `://` is read as `http://` (curl does the same), and a URL with a space or a control character is exit code 3 before anything is sent (curl: "URL rejected"). The check is in the command, not in `httpp::url`: the URL parser still accepts a space in a path, which a library caller meets as `invalid request line` from the server. ponytail: moving the check into `url_is_http` is the better home and is not done here.
- Default `User-Agent` of `httpp curl` is `curl/8.18.0`, the curl release whose behavior the command follows (`k_curl_compat_version`); `-A` and `-H` win. `httpp curl --version` and `-V` print `httpp curl <version> compatible with curl <curl version>`. `httpp::request` keeps its own User-Agent: only the command presents itself as curl, because it is meant to replace it in scripts (parrot.live, for one, streams only to `curl/...` and redirects every other agent).
- The documentation gets a page, *The httpp curl command*, and the server pages use `httpp curl`. Their `# HTTP/1.1 201 Created` comments lose the reason phrase, because `httpp curl -i` prints `HTTP/1.1 201`.

## Consequences

- `-o` changed from one value to a list; one `-o` with one URL works as before.
- Still missing: `-F`, `--json`, `-G`, `-b`, `-x`, `-k`, `--cacert`, `--data-binary`, `-C`, `-r`, `--http2`. `httpp::request` has proxies, cookies, TLS options, JSON and form bodies; the command does not expose them yet. `-m` still limits each step, not the whole transfer. The command still collects the whole body in `response::body` and writes it at the end, so an endless answer prints nothing and a big file sits in memory; the fix is to write each piece as it arrives (a public streaming hook on `request`, dual in C++ and Python, built on the engine's `body_sink` of ADR-0052). Not done yet.

## Related

ADR-0077, ADR-0079, ADR-0083
