# ADR-0110: `retry_delay_ms`, and a `retry_delay` that takes a fraction in Python

- **Status:** implemented in C++ and Python (built and tested on Linux); Windows and macOS not run here
- **Date:** 2026-11-23
- **Decided by:** the project owner. A test needed to wait 50 ms for a server to start and `retry_delay` could not (its smallest wait is one second). The owner chose to keep seconds and add a `_ms` twin ("the second stays, `ms` is added").
- **Source:** `include/httpp/client.hpp` (`retry_delay_ms`), `src/core/http_exchange.cpp` (`wait_before_retry_ms`, `set_retry_delay_ms`), `src/core/client.cpp`, `src/bindings/python/cy/{client,transfer,declarations}.pxi`, `tests/cpp/test_client_retry.cpp`, `tests/python/test_request_streaming.py`
- **Follows:** ADR-0097, ADR-0109

## Decision

1. **The wait is stored in milliseconds** (`retry_delay_ms` in `session_settings` and `request_spec`). `retry_delay(long seconds)` keeps its meaning and sets `seconds * 1000`.
2. **`retry_delay_ms(long ms)` is new** on everything that has `retry_delay` (`request`, `client`, `async_client`, `download`). One setting with two spellings: the last one set wins. 0 is still the default back-off of 1, 2, 4, ... seconds (600 at most). A negative number, or more than a day, throws `std::invalid_argument` (`ValueError` in Python). The day limit is the one the CLI already had for `--retry-delay`.
3. **The getters:** `retry_delay_ms()` gives what was set; `retry_delay()` gives whole seconds, rounded down (200 ms reads as 0).
4. **`on_retry` keeps its type** and is told whole seconds, rounded up (a 100 ms wait is announced as 1). Changing the type to milliseconds would break every caller; the notice is for people, and nobody who needs the exact value has asked.
5. **Python:** `retry_delay(seconds)` takes an int or a float (`0.2` is 200 ms), where it used to round to a whole second (0.4 was 0, the default back-off). `retry_delay_ms(ms)` is new, and `retry_delay_ms=` is a keyword of `Request`, `Download`, `Client` and `AsyncClient` as ADR-0100 asks. Giving both keywords at once is a `ValueError`.
6. **The CLI is unchanged:** `--retry-delay` stays in seconds, like curl's.

## Rejected alternatives

- **Turn `retry_delay` itself into milliseconds:** `retry_delay(2)` in existing code would silently become 2 ms.
- **`retry(5, true, 0.5)` (one call with positional arguments):** a bare `true` says nothing at the call site. If one call is wanted, keyword-only arguments in Python (`retry(5, connrefused=True, delay=0.5)`) are readable; not done here.
- **A separate wait helper in each test:** the library already retries, and a refused connection is the case `retry_connrefused` is for.

## Consequences

- `Request(url).retry(100).retry_connrefused().retry_delay_ms(50).run()` waits for a server that is still starting, at most about 5 s. It also tries again after a timeout and the transient statuses of ADR-0097 (a handler that answers 503 runs again), so use it for a request that is safe to repeat.
- `settings_ref` gains two members: rebuild the consumers.
