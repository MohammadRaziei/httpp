# ADR-0110: `timeout`, `connect_timeout`, `max_time` and `retry_delay` take a fraction of a second

- **Status:** written but not built or run (the owner asked for the code first and the build later); Windows and macOS not run either
- **Date:** 2026-11-23
- **Decided by:** the project owner. A test needed to wait 50 ms for a server to start, and `retry_delay` could not (its smallest wait was one second). A first version added `retry_delay_ms` next to `retry_delay`; the owner preferred one name, and then asked that every wait of the client be a `double`: "`timeout` and the others should all become double".
- **Source:** `include/httpp/client.hpp` (`settings_ref`, `configurable`), `src/core/internal/http_exchange.{hpp,cpp}` (`set_*`, `check_wait`, `wait_before_retry`), `src/core/internal/async_http.{hpp,cpp}` (`seconds_to_ms`, `async_job::timeout_sec`), `src/core/client.cpp`, `src/core/download.cpp`, `src/bindings/python/cy/{client,transfer,declarations}.pxi`, `tests/cpp/test_client_retry.cpp`, `tests/cpp/test_client_max_time.cpp`, `tests/python/test_request_streaming.py`
- **Follows:** ADR-0092, ADR-0093, ADR-0097, ADR-0109

## Context

Python's server side already took `handler_timeout=0.3`, while the client rounded every wait to a whole second: `Request(url).timeout(0.5)` silently waited 1 s (`max(1, round(...))`), and `retry_delay(0.4)` meant 0, the default back-off of 1, 2, 4 s. The C++ settings were `long` seconds, so a short wait could not be asked for at all.

## Decision

1. **`timeout`, `connect_timeout`, `max_time` and `retry_delay` are `double` seconds,** in `request`, `client`, `async_client` and `download` (setters and getters, through `settings_ref`) and in Python. `timeout(30)` and `retry_delay(2)` mean what they meant; `timeout(0.5)` is half a second. The getters return what was set.
2. **The meaning of 0 does not change:** `timeout` 0 (or less, or not a number) is the default (300 s), `connect_timeout` 0 is "`timeout` covers the connect steps", `max_time` 0 is no limit, `retry_delay` 0 is the back-off of 1, 2, 4, ... seconds (600 at most).
3. **Checks:** a negative number or not-a-number throws `std::invalid_argument` (`ValueError` in Python) for `connect_timeout`, `max_time` and `retry_delay`; `retry_delay` above a day throws (the limit the command line already had) and the other two above `longest_wait_seconds` (1e9 s, about 31 years, which keeps `now() + wait` inside the range of `steady_clock`). `timeout` keeps its old rule (0 or less is the default) and is cut at the same top.
4. **One conversion for timers,** `detail::seconds_to_ms(double)`: milliseconds rounded to the nearest, and at least 1 for anything above 0, so a tiny wait never becomes "no wait". The engine's timers, the `max_time` deadline, the retry timer and the download's lock-wait all use it.
5. **Python:** the `max(1, round(...))` is gone; a float goes through as it is. One visible change: `Request(url).timeout(0)` (and `Download.timeout(0)`) used to be 1 s and is now the default, as in C++ ("0 or less: the default").
6. **Not changed:**
   - `on_retry` keeps `long delay_seconds` and is told whole seconds, rounded up (a 50 ms wait is announced as 1): a lambda taking `long` would otherwise cut a `double` silently.
   - The CLI flags (`--retry-delay`, `--max-time`, `--connect-timeout`) stay whole seconds. curl takes a decimal for the last two; not done here.
   - `fetch_public_suffix_list(..., long timeout_seconds)` and the server-side timeouts (`int` seconds, `handler_timeout`) are other settings.

## Rejected alternatives

- **`retry_delay_ms` next to `retry_delay` (the first version of this ADR):** two names for one setting need a rule for "both given" and a second thing to document.
- **Turn `retry_delay` into milliseconds:** `retry_delay(2)` in existing code would silently become 2 ms.
- **`retry(5, true, 0.5)` (one call with positional arguments):** a bare `true` says nothing at the call site. If one call is wanted, keyword-only arguments in Python (`retry(5, connrefused=True, delay=0.5)`) read better; not done here.
- **`std::chrono::duration` parameters:** correct in C++, but Python and the command line have no such type, and `timeout(30)` would stop compiling.

## Consequences

- `Request(url).retry(100).retry_connrefused().retry_delay(0.05).run()` waits for a server that is still starting, at most about 5 s. It also tries again after a timeout and the transient statuses of ADR-0097 (a handler that answers 503 runs again), so use it for a request that is safe to repeat.
- The signatures of `settings_ref` changed (`long` to `double`): **rebuild the consumers.** Source code that passes an `int` or a `long` compiles as before.
- `request_spec`, `session_settings` and `async_job` store seconds as `double`; the engine tests that write `job.timeout_sec = 10;` compile unchanged.
- Not run here: the whole change (no build was asked for), in particular the Cython declarations and the C++ and Python tests added with it.
