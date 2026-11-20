# ADR-0098: `httpp curl` gets `-k`, `--cacert`, `-J`, `--output-dir`, `--create-dirs`, `--json` and `--fail-with-body`; the pieces live in the library

- **Status:** accepted
- **Decided by:** the project owner ("implement the high-priority ones", and "add the capabilities to `httpp::request` as far as possible, not to the CLI")
- **Source:** `include/httpp/path.hpp`, `include/httpp/url.hpp`, `include/httpp/client.hpp`, `src/cli/httpp_cli.cpp`, `docs/site/docs/client/curl-command.mdx`
- **Follows:** ADR-0084, ADR-0097

## Context

`httpp curl` already had `-X`, `-x`/`--noproxy` and the proxy variables (ADR-0097). What was left of the common curl flags was reaching a server with a certificate of your own, naming and placing the downloaded file, sending JSON, and getting the error body of a failed request.

## Decision

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

## Consequences

- No new `request` method was needed; the transfer features of `request` this CLI uses are all the ones it had.
- The three library helpers are inline in their headers, so the C++/Python parity test does not list them. `Response.filename` is not in Python yet.
- `-v` still shows only the headers `httpp curl` sends, not the engine's own.

## Related

ADR-0084, ADR-0097
