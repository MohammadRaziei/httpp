# ADR-0083: `httpp curl` behaves like curl on errors, and `httpp/curl_compat.h` is retired

- **Status:** accepted
- **Decided by:** the project owner ("those gaps must be fixed", "retire `curl_compat` too", "keep presenting httpp as the modern replacement for curl, as for aiohttp, httpx and requests")
- **Source:** `src/cli/httpp_cli.cpp`, `tests/python/test_cli.py`; removed: `include/httpp/curl_compat.h`, `src/core/curl_compat.cpp`, `tests/cpp/test_curl_compat.cpp`; edited: `include/httpp.h`, `README.md`, `tests/cmake/consumer_project/main.cpp`
- **Follows:** ADR-0077 (its sentence that `curl_compat.h` stays is replaced by this ADR)

## Context

After ADR-0077 `httpp curl` could show heads and follow redirects, but it still differed from curl where scripts notice it: every non-2xx answer exited with 1, a failed transfer said only `status 0` and exited 1 whatever the cause, it left an empty `-o` file behind, and only one `-d` was accepted.

`httpp/curl_compat.h` was a 15-option `curl_easy_*` subset with its own error numbers. Nothing in the project used it besides its test and the installed-package smoke test, and it could not deliver "change the header, keep the code" (no auth, proxy, TLS options, cookies, `multi`), so it promised more than it kept and had to follow every change of `httpp::request`.

## Decision

- An HTTP answer is not a failure: any status exits 0 and prints its body. `-f/--fail` turns a status of 400 or more into exit 22 with no output and no `-o` file (3xx is not an error, as in curl).
- A transfer with no HTTP answer prints `error: <what happened>` (the reason from `response::error_message` when it adds something) and exits with curl's code: 3 `invalid_url`, 7 `connection`, 28 `timeout`, 35 `tls`, 47 `too_many_redirects`, 63 `too_large`, 1 for the rest. No `-o` file is written. `connection` covers a failed lookup too, because the engine does not report it separately.
- Several `-d` are joined with `&`; the URL may stand before or after the options.
- `httpp/curl_compat.h`, `curl_compat.cpp` and their test are removed without a replacement. Code that used `curl_easy_*` moves to `httpp::request` (Python: `httpp.Request`). The README presents `httpp curl` and `httpp::request` as a modern counterpart of curl, in the same way httpp is presented next to aiohttp, httpx and requests, and says plainly which curl flags are missing.

## Consequences

- **Behaviour change:** a script that relied on `httpp curl` failing for 404 must pass `-f`. Scripts that tested for exit code 1 on a refused connection now see 7.
- **Breaking for C++ code (major is 0):** `#include "httpp/curl_compat.h"` and every `curl_easy_*`, `CURLOPT_*`, `CURLINFO_*`, `CURLE_*` name are gone.
- `httpp download` is unchanged (it still exits 1 on a non-2xx answer): it is a download command, not curl.
- Still not in `httpp curl`: `-s`, `-S`, `-w`, `-A`, `-e`, `-O`, `-T`, `-D`, `-d @file`, `--compressed`, `--retry`, `--connect-timeout`, several URLs. `httpp::request` is to be enriched first (decided direction, the first additions are not chosen yet).

## Related

ADR-0041, ADR-0053, ADR-0062, ADR-0067, ADR-0077
