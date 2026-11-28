# ADR-0114: A streamed body is read one piece ahead, and a served file is opened once

- **Status:** implemented in C++ and built on Linux: the 3 new tests of `tests/cpp/test_async_server.cpp`, the whole C++ suite. Measured on Linux, not on Windows or macOS.
- **Date:** 2026-11-28
- **Decided by:** the project owner, after the benchmark part `file` showed httpp no faster than a bare ASGI app for a 256 KiB file, and he asked for the cause to be fixed in the most principled way.
- **Source:** `src/core/async_server.cpp` (`pump`, `fetch`, `fetched`, `piece_written`, `piece_ready`, `stream_state`), `src/core/server.cpp` (the file answer)
- **Follows:** ADR-0077 (streamed responses), ADR-0045 (validators of a file answer)

## Context

A streamed body (a file, or a generator) is read on a pool thread, because the reader may block. The engine did this one piece at a time, in lock step: ask a pool thread for a piece, hand it to the loop, write it, and only when the write is done ask for the next one. Reading and writing never overlapped, and every piece cost a hand-off to a pool thread and one back.

Measured with `strace -f -c` over 1800 requests on one connection (Linux, 256 KiB file, `io_threads=1`):

| route | futex calls per request | epoll_wait | sendto |
|---|---|---|---|
| `hello` (`def`) | 7.2 | 3 | 1 |
| file (`def`) | 23.1 | 9 | 5 |

Sixteen futex calls more than a plain answer, for two pieces. The server CPU per request in the benchmark (75 us against 49 us for a bare ASGI app that reads the file in the handler) agrees. The file path also opened the file twice (`file_size()` opened it to test that it can be read, then the answer opened it again).

## Decision

1. **One piece is read ahead.** When a piece arrives and more are to come, the read of the next piece starts at once, before the write of this one. When that write is done, the piece read meanwhile goes out, or goes out when it arrives. There are never two reads at a time, so the generator is still called one piece at a time and in order; it is only called earlier. At most one piece waits (`stream_state::ahead`), so a streaming connection holds at most two pieces instead of one.
2. **The file is opened once.** `stat` tells a regular file from a directory (which opens without an error on Linux), the open tells a readable file from one that is not. The answers (404 for both) are the same as before.
3. **The first piece is not touched.** It is still asked for after the head is written.

A body that ends early, or throws, on the piece read ahead is noticed after the piece before it has been written, as before: the client gets every piece that was good and then the connection drops (`a_stream_that_throws_midway_drops_the_connection_without_an_ending` is unchanged).

## Rejected alternatives

- **A bigger piece (1 MiB instead of 128 KiB).** Measured: futex calls fell from 23 to 13 per request, but a 256 KiB file got slower (about 200 to 280 us per request on one connection), because the first byte waits for the whole read and nothing overlaps. Fewer hand-offs is not the same as less waiting.
- **`sendfile(2)`.** The best answer for a plain socket (no copy, no pool thread), but it needs the native handle and a wait for the socket to be writable, a different call on macOS, none on Windows (`TransmitFile`), and it does not help a TLS connection or a compressed answer. It is a separate decision with a fall back to this path, not a replacement for it.
- **Read the first piece on the loop thread.** It would save one hand-off, and it would let a cold disk block the loop for every other connection. The engine does not block its loop thread.

## Consequences

- A generator for `res.stream` is now advanced one piece before the client has taken the previous one, never more. A producer with side effects per piece sees the next call earlier (still in order, still one at a time). A slow client holds back the generator by at most one piece.
- Memory per streaming connection: one more piece (128 KiB for a file).

## Tests

- `a_stream_reads_one_piece_ahead_of_a_write_that_cannot_finish_and_no_further`: 24 MiB pieces and a client that reads nothing. The generator has been called twice (one piece stuck in the socket, one ahead) and not three times, then all four pieces arrive whole and in order. Fails on the old `pump` (called once).
- `many_small_pieces_arrive_in_order_and_the_connection_stays_usable`: 300 chunks, twice on one connection.
- `a_client_that_leaves_while_the_next_piece_is_being_read_releases_the_generator`: the generator and the piece read ahead are released while the server still runs.

## Measured

On one connection with a Python client, Linux, 150 requests of a 4 MiB file (32 pieces), medians of 9 runs, old build against new, alternating: 4363 and 4726 us before, 3713 and 3682 us after (about 15 to 22 percent less). For a 256 KiB file (2 pieces) the difference was inside the noise of the machine (265 to 481 us for the same build): not shown. These are latency numbers of one connection, not the throughput of the benchmark part `file`; that has to be run again (`benchmarks/run.sh full`).

## Not verified

Windows and macOS (the code is portable Asio and standard C++, but only Linux was built and run), TLS connections (the same code path, no separate test), the throughput of the benchmark part `file` after this change.
