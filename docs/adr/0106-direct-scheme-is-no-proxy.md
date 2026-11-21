# ADR-0106: `direct://` means no proxy, like `""`, when a proxy is given explicitly

- **Status:** implemented in C++ (built and tested on Linux); the Python and CLI tests are written but were not run here; the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-21
- **Decided by:** the project owner asked for a word that switches the proxy off in the CLI and in code, the same as `""`. The implementer first tried the bare word `off`, then `direct`; the owner then proposed `direct://`, and asked for the url parser to be checked before it was built.
- **Source:** `src/core/http_exchange.cpp` (`is_proxy_direct_url`, `parse_proxy_url`), `src/core/proxy.cpp` (`resolve_proxy`, `proxy_url`), `include/httpp/client.hpp`, `include/httpp/proxy.hpp`, `src/cli/httpp_cli.cpp` (help text), `tests/cpp/test_proxy.cpp`, `tests/cpp/test_async_pool_proxy.cpp`, `tests/python/test_proxy.py`, `tests/python/test_cli.py`
- **Follows:** ADR-0067, ADR-0068, ADR-0077

## Context

An empty proxy has meant "no proxy at all" in two places since ADR-0067: `parse_proxy_url` (behind `proxy(url)` on `request`, `client`, `async_client` and `download`) and `resolve_proxy` (behind `httpp curl -x ""` and Python's `resolve_proxy`). `""` is easy to get wrong and hard to read in a shell line or a config file.

A bare word (`off`, `direct`) does not fit the proxy value. Every value so far is a URL whose scheme says how to reach the target (`http://`, `socks5://`, `socks5h://`). A bare word has no scheme, so it is read as a host (`proxy_url` puts `http://` in front of any text without `://`), and taking one word as "no proxy" takes that host name away. A scheme has no such cost.

What `liburlparser` does with the text, measured with a small program, not assumed (the same results on the pinned `ab454b2` and on 2.3.1, which only changed `abspath()`):

- `direct://` gives scheme `direct`, no host, port 0, no user, empty path, query and fragment. The scheme is lower-cased (`DIRECT://` is the same) and the text is trimmed.
- `direct:`, `direct:/` and `direct://` all give the same url, so the url alone cannot tell them apart.
- `direct://x` has the host `x`; `direct://:3128` has port 3128; `direct://u@` has the user `u`; `direct://?a=1` and `direct://#f` have a query and a fragment; `direct:///x` has the path `/x`. None of these is "no proxy".
- A bare `direct`, `off` or `direct:3128` has an empty scheme (the text lands in the path). `http://direct` has the host `direct`.
- **`str()` is not the text that was given.** It is normalized: `direct://` becomes `direct:/`, `direct://x` becomes `direct:x/`, `http://` becomes `http:///`. A check on `str()` would work for `""` (it stays empty) and for bare words (they stay as written) and would silently fail for a scheme. The check has to be made on the parsed parts.

## Decision

1. **One predicate, `detail::is_proxy_direct_url(const url&)`,** in `internal/http_exchange.hpp`. It is true when the scheme is `direct` and there is no host, no port, no user, no password, no query, no fragment and the path is empty or `/`. It reads the parsed parts and never `str()`.
2. **Where it is used.** `parse_proxy_url` returns "no proxy" for it, which covers `proxy(url)` on all four C++ classes. `resolve_proxy` checks the explicit value with it before anything else, which covers `httpp curl -x/--proxy` and Python's `resolve_proxy`. The CLI only changed its help text.
3. **Anything else with the scheme `direct` is an error** with a message of its own (`direct:// turns the proxy off and takes nothing after the scheme ...`), not the generic "not an http:// URL" one: that one would print the normalized text (`direct:somehost/`), which the user never wrote.
4. **Explicit values only.** `proxy("direct://")`, `-x direct://` and `resolve_proxy(target, "direct://", ...)` mean no proxy and ignore the environment, as `""` does. In an environment variable `direct://` is an error (`proxy_url` refuses it) instead of being returned as a proxy with an unusable scheme; before this ADR it was an error too (unknown scheme), so nothing changes there.
5. **The bare words are hosts again.** `direct`, `off` and `direct:3128` are what they were: a host called `direct` or `off`. Nothing is taken away.
6. **`direct:` and `direct:/` work like `direct://`,** because the parser does not tell them apart and the text is not kept. This is harmless leniency and is covered by a test, so that it is a decision and not an accident.

## Taken by default while implementing (owner to confirm)

- **`curl -x direct://` is an error in real curl** (an unknown scheme), so a command that worked in curl never changes meaning here; httpp only gives meaning to a value curl refuses. This is from the curl documentation as I remember it; I did not run curl here.
- **The environment is not changed** (decision 4). Reading `http_proxy=direct://` as "no proxy", and skipping `all_proxy` for that scheme, would be a new feature; it can be added separately.
- **`--noproxy '*'` is still the curl-compatible way** to send everything direct. It keeps `-x` set and lets the list decide; `-x direct://` drops the proxy before the list is read.

## Rejected alternatives

- **The bare word `off`, then `direct`** (tried first): each takes a host name away, `direct` more likely than `off` (intranets name machines by role), and neither fits a value that is otherwise a URL.
- **Both a word and `direct://`:** two spellings of one thing.
- **Judge the text, not the parsed url** (compare the given string with `direct://`): it would work for `resolve_proxy`, which has the text, but not for `proxy(const url&)`, which gets a url whose text is already rewritten. Two rules would have to be kept equal.
- **A named constant or a `proxy_direct()` function:** more API for one string, and it would not help a shell line or a config file.

## Consequences

- `client.proxy("direct://")`, `request.proxy("DIRECT://")` and `httpp curl -x direct://` work. Nothing that worked before changes meaning.
- Nothing changes in the Python binding: its `proxy` keywords and `resolve_proxy` pass the text on, and a bad value reaches Python as the same `ValueError`.
- Not run here: the Python suite and Windows and macOS.
- Found while checking the parser: `abspath()` dropped a trailing slash (`/dir/` gave `/dir`), which made `url_filename("http://h/dir/")` return `dir` and the test `httpp_url.url_filename_is_the_last_piece_of_the_path` fail. liburlparser 2.3.1 fixes it in the parser (the right place, a generic RFC 3986 rule); httpp only has to move its pin from `ab454b2` to `v2.3.1`. With 2.3.1 the whole C++ suite passes (768 of 768).
- Not decided here: `str()` of a non-special scheme with a host drops the `//` (`direct://x` gives `direct:x/`), which is not what WHATWG serialization does as far as I remember. httpp does not depend on it (decision 1), so it is left for the parser.
