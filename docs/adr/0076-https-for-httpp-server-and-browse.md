# ADR-0076: `httpp server` and `httpp browse` can serve HTTPS

- **Status:** accepted
- **Decided by:** the project owner (asked for TLS on the two commands that stay up)
- **Source:** `src/cli/httpp_cli.cpp` (`tls_args`, `add_tls_options`), `src/core/tls_session.cpp` (the encrypted-key hint), `tests/python/test_cli.py` (the TLS section), `docs/site/docs/server/serve-directory.mdx`, `docs/site/docs/server/browse-command.mdx`

## Context

The core has served TLS since ADR-0040 (`server::set_tls(cert, key, password)`), and `httpp run` exposes it as `--ssl-certfile`, `--ssl-keyfile` and `--ssl-keyfile-password`. The two native commands that keep a server up, `server` and `browse`, had no way to use it, so sharing a folder over a network meant plain HTTP.

## Decision

- Both commands get the same three options as `httpp run`, with the same names: `--ssl-certfile FILE`, `--ssl-keyfile FILE`, `--ssl-keyfile-password PASSWORD`. One shared helper adds them, so the two commands cannot drift apart.
- The certificate and the key come together: CLI11 refuses one without the other (exit 2), and refuses a file that does not exist (exit 2).
- The password also comes from `HTTPP_SSL_KEYFILE_PASSWORD`, so it can stay out of the process list (a command-line argument shows in `ps`). The option keeps the `httpp run` name and still works.
- `set_tls` runs while the server is set up, before it listens. Its errors (unreadable file, key not belonging to the certificate, wrong password) end the command with exit 1 and its message, which names the file. An encrypted key given without a password used to fail as "PEM expected"; `set_tls` now says the key is encrypted and no password was given (ADR-0078 moved that hint from the CLI into the core).
- The address the command prints says `https://` when TLS is on (`server::url()`, ADR-0078). The core's own `listening on https://...` log line already did.

## Rejected alternatives

- **Short names (`--cert`, `--key`).** Nicer to type, but a third spelling next to the two the project already has for the same thing. The names follow `httpp run`.
- **A `--key-password-file` option, no password argument.** Safer on its own, but it would be the only place the three commands differ. The environment variable gives the same protection with the existing name.
- **`--self-signed` to make a certificate on the fly.** Useful, but it is a feature of its own (key type, lifetime, names, where to keep it) and the core has no certificate generation. The documentation shows the one-line `openssl` command and `mkcert` instead.
- **Serving HTTP and HTTPS together, or redirecting.** One port serves one of them (ADR-0040); a second listener is a different command line and more state.

## Consequences

- A plain `http://` request to a TLS port is dropped, as ADR-0040 says; the tests check it does not answer 200.
- The files are read once at start-up; a renewed certificate needs a restart (ADR-0040).
- The "encrypted key" hint is a search for `ENCRYPTED` in the key text, not a parse. The PKCS#8 `ENCRYPTED PRIVATE KEY` and the legacy `Proc-Type: 4,ENCRYPTED` forms both contain it; a key that does not would get the plain message.
- Not verified: Windows and macOS (the tests ran on Linux with the openssl command line making the certificate); a real browser's trust prompt for a self-signed certificate; certificates with a chain longer than the leaf; client certificates (the core has none).

## Related

ADR-0040 (TLS in the core), ADR-0041 (the `httpp` command), ADR-0073 (the two commands' options).
