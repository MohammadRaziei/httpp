# ADR-0097: `response::elapsed_ms` and `response::bytes_received`, so the curl command does not time its own call

- **Status:** implemented in C++ (built and tested on Linux); Python is not bound yet
- **Decided by:** the owner asked that `httpp curl` be a thin call into `httpp::request` and that logic such as the timeout report live in the library
- **Source:** `include/httpp/client.hpp` (`response`), `src/core/http_exchange.cpp` (`run_chain`), `src/core/async_http.cpp` (`exchange`), `src/cli/httpp_cli.cpp` (`curl_transfer`)
- **Follows:** ADR-0084, ADR-0094

## Context

The last commit made `httpp curl` print `error: timed out after 2003 ms with 120 bytes received`, like curl does, so a user can tell a server that never answered from one that was still sending. To do that `curl_transfer` kept its own `std::chrono::steady_clock::time_point attempt_started`, set it before every attempt, and counted the body bytes in its `on_body` callback (`downloaded`).

That was logic of the request in the front end. The library already runs the clock (`max_time` is measured from the first hop) and already sees every body byte, but it did not say what it saw. Any other caller (a C++ program, the Python bindings later) that wanted the same report had to time and count by itself, and could not do it as exactly as the engine, because the engine's clock includes the queue and the redirect hops.

## Decision

1. `httpp::response` has two client-side fields, filled on success and on failure:
   - `long long elapsed_ms`: from the call to `run()` (or `run_async()`) until the last byte or the failure, redirects included. It is the same span `max_time()` limits, so a request that hit `max_time(1)` reports a little over 1000.
   - `std::size_t bytes_received`: body bytes of the final response that arrived, before gzip or deflate decoding. With `on_body()` it is the sum of the pieces the callback was given (a piece is counted before the callback runs, so a callback that stops the transfer is included). Without it, it is what was collected into `body`. A redirect that is followed does not count, and a skipped body (`on_head` returned false, or a HEAD) is 0.
2. The clock starts in `run_chain`, once per request, and stops in a wrapper around the callback that receives the final response. The recursion over redirect hops moved into `run_hops` so the wrapper is added once, not per hop. Every path (`run()`, `run_async()`, `client`, `async_client`, `download`) goes through `run_chain`, so none needs its own code.
3. `bytes_received` is a plain counter on the engine's `exchange`, next to the code that appends or forwards the body. The engine's own retry on a dead kept-alive connection resets it with the rest of the response.
4. `httpp curl` reads the two fields. `attempt_started` and `downloaded` are gone from `curl_transfer`, and `-w %{size_download}` is `res.bytes_received`. The wording of the message stays in the CLI: it imitates curl and is output formatting, not behaviour of the request.
5. The text of `response::error_message` does not change (`"timed out"` for a step, `"total time limit reached"` for `max_time`), so nothing that matches on it breaks.

## Rejected alternatives

- **Put the whole sentence into `error_message`** (`"timed out after 2003 ms with 120 bytes received"`). It would change a string callers may match on, and the CLI would have to avoid printing the label and the message twice. Numbers are easier to use than a sentence, and the sentence can be made from them.
- **A separate `timing` struct on `response`.** More names for two numbers, and the Python and C++ parity work (ADR-0094) would grow with it.
- **`std::chrono::milliseconds` as the type.** It would put `<chrono>` into the public header and the Python binding would have to convert it. A plain count with the unit in the name is what `timeout(long seconds)` and `max_time(long seconds)` already do.
- **Retry inside `request` (`--retry`)** was considered in the same pass and left out. It is a larger public API (`retry`, `retry_delay`, `retry_connrefused` on all three classes of ADR-0094, plus Python) and needs its own decision.

## Consequences

- `response` grows by 16 bytes. It is an exported struct: **rebuild the consumers** (same note as ADR-0094).
- The CLI has no clock and no byte counter of its own; `curl_transfer` only formats what the response says.
- Python's `Response` does not expose the two fields yet. The Cython bindings were not compiled in this change.
- `elapsed_ms` counts from the call, so it includes time spent waiting for a free connection in `async_client`; it is the time the caller waited, which is what a timeout report is about.
