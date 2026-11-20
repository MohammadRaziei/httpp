# ADR-0099: `retry`, `retry_delay`, `retry_connrefused` and `on_retry`: sending again is the library's job, not the curl command's

- **Status:** implemented in C++ (built and tested on Linux); Python is not bound yet (listed in `NOT_EXPOSED`)
- **Decided by:** the owner asked that `httpp curl` be a thin call into `httpp::request` and that its logic live in the library; after ADR-0097 he told me to move `--retry` too
- **Source:** `include/httpp/client.hpp` (`configurable`, `settings_ref`, `request::on_retry`), `src/core/http_exchange.cpp` (`run_chain`, `run_tries`), `src/core/async_http.cpp` (`post_after`), `src/cli/httpp_cli.cpp` (`fill`)
- **Follows:** ADR-0084, ADR-0094, ADR-0097

## Context

`httpp curl --retry` was a loop in `curl_transfer`: it knew which answers may pass on another try (a timeout, 408, 429, 500, 502, 503, 504, and a failed connection with `--retry-connrefused`), computed the wait (1, 2, 4, ... up to 600 seconds), slept on the calling thread, and had a line in its `on_head` so that an answer about to be sent again was not written to the output. Every other caller of the library had to write the same loop, and could not get the last part right for a streamed body: only the library knows which answer is the last one before it hands the head to `on_head()`.

## Decision

1. Three settings in `configurable` (so on `request`, `client` and `async_client`, and a session hands them to the requests it makes), named like the curl flags:
   - `retry(int times)`: more tries after one that may pass. 0 (the default) never sends again. A negative number throws `std::invalid_argument`.
   - `retry_delay(long seconds)`: the wait before each new try. 0 (the default) is 1, 2, 4, ... seconds, 600 at most (`min(600, 2^n)`, n counting from 0). A negative number throws.
   - `retry_connrefused(bool)`: also send again after `error_kind::connection`. Off by default.
2. `request::on_retry(fn)`, with `fn(const response& last, int retry_number, long delay_seconds)`: called before each new try, before the wait begins. It is a per-request callback like `on_body`, not a setting, so it is on `request` only. It runs on the network thread; a callback that throws is ignored.
3. Which answers are worth another try is decided once, in `run_tries` (`src/core/http_exchange.cpp`): `error_kind::timeout`, the statuses 408, 429, 500, 502, 503 and 504, and `error_kind::connection` only with `retry_connrefused`. A redirect that is followed is not an answer and is not a try; a 404 never is retried. Same table as curl.
4. Every try is a whole request: it gets a copy of the spec, its own `max_time()` clock and its own redirect count, so `timeout()` and `max_time()` limit a try, not all of them together. The returned response is that of the last try, `elapsed_ms` and `bytes_received` (ADR-0097) included. The calls without `retry` take the old path unchanged.
5. With `on_body()` the sink of a try that can still be followed by another is wrapped: a transient status is answered with `verdict::skip`, so the body is read and thrown away and neither `on_head` nor `on_body` of the caller sees it. On the last try the head is handed on as usual. A transient answer is therefore shown to the callbacks if and only if it is the final one. A failure after a body already reached `on_body` (a timeout in the middle of it, say) is still retried and the body then arrives again from its first byte: the callbacks cannot take it back. `httpp curl` behaves the same as before.
6. The wait is a timer, not a sleep: `async_http::post_after(delay, fn)` runs `fn` on the engine's loop thread when the time has passed, so other requests go on, and `run_async` and `async_client` need no thread of their own. `run()` still blocks its caller for the wait, as it blocks for the request. The wait holds the connection slot of an `async_client`.
7. `httpp curl` copies `--retry`, `--retry-delay` and `--retry-connrefused` into those settings and, unless `-s`, prints `Warning: transient problem, will retry in N seconds (K of M)` from `on_retry`, as before. It has no loop, no clock, no transient table and no sleep of its own, and no `#include <thread>`.
8. The wait is in whole seconds (curl's unit); `retry_delay` takes no fraction.

## Rejected alternatives

- **A `retry` policy object** (`retry_policy{ .times, .delay, .on }`). More names and a new type for three plain values, and ADR-0094 already has a place for session settings. Rejected for the same reason as a `timing` struct in ADR-0097.
- **A caller-supplied predicate** (`retry_if(fn)`). curl has a fixed list and so does every script this command replaces; a predicate can be added later without changing these three names.
- **Retry only in `run()`** (a loop with `sleep_for`). `run_async()` and `async_client` would then ignore a setting that `configurable` gives them, and a sleeping `done` callback would freeze the shared network thread for every other request.
- **A detached thread per wait.** Works, costs a thread per waiting request; the engine already owns a loop with timers.
- **`unlimited` for `retry`** (ADR-0095). Retrying forever is not a limit being lifted but a different policy; curl's `--retry` takes a number too.
- **Python in the same change.** The bindings were not compiled (no Cython in the sandbox), so the five names are listed in `tests/python/test_cpp_python_parity.py` `NOT_EXPOSED` with this ADR, as `on_body` is, and bound next.

## Consequences

- `session_settings`, `request_spec` and the exported `settings_ref` grow: **rebuild the consumers** (same note as ADR-0094 and ADR-0097).
- `httpp curl --retry` keeps its flags, its warning text and its exit codes; what changed is who sends the request again.
- A C++ program, and later Python, gets `--retry` without a loop of its own, and a streamed download never shows a transient answer by accident.
- An engine destroyed while a retry waits never fires its timer. Both engines of the process are leaked on purpose (see `async_http.hpp`), so this cannot happen today; a new short-lived engine would have to cancel.
