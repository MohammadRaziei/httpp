# ADR-0036: The per-connection read buffer is not zero-filled

- **Status:** accepted
- **Source:** `src/core/async_server.cpp` (`conn::rbuf`), `benchmarks/diag_io.sh`

## Context

Each connection owned `std::string(16384, '\0')` as its read buffer: 16 KiB written with zeros when the connection is made. `perf record -e cache-misses` of `cpp_bench /io`
on the owner's machine (2026-11-04, 4 I/O threads, 64 connections, `benchmarks/diag` runs) showed `__memset_avx2_unaligned_erms` at 4.2 percent of all cache misses and
`async_server::impl::adopt` at 1.3 percent; Drogon's profile has no such entry. Both servers are dominated by the kernel (`kernel_init_pages` 26 and 33 percent of the misses).
In the benchmark a connection ends after `keep_alive_max` (default 1000) requests and the load generator opens a new one, so the zeroing is paid regularly; in a server with many
short connections or many idle ones it is paid per connection and also makes every idle connection touch all four pages (the sandbox measured about 20 KiB per idle connection
against about 9 KiB for Drogon, unchecked on the owner's machine).

## Decision

The buffer is an uninitialized `char[16384]` (`unique_ptr<char[]>`), size kept in a constant. `recv` writes only the bytes it receives and every reader uses the count it returns,
so no code depends on the zeros. No API change.

## Consequences

- Estimated effect, not measured: the memset is about 5 percent of the cache misses, which is about 0.3 misses per request at `keep_alive_max` 1000 (256 lines per 1000 requests).
  The gap to Drogon in cache misses per request (7.5 against 2.9, `perf stat`) is about 4.6, so this does not explain it. Idle memory per connection should drop towards what a
  connection really touches; **not measured here**.
- The 7 to 10 percent throughput gap to Drogon at 4 I/O threads is still unexplained. Open readings: the `perf stat` cache-miss counts (7.5 and 2.9) and the sampled profiles
  do not agree on how big the gap is; the context-switch count of `perf stat` (8619 against 216) was not reproduced with sampling (both servers sleep in `epoll_wait` at
  the same share, 87 and 98 percent of their switches); a fair comparison should run `cpp_bench` with `keep_alive_max` 0 as the third argument.
- Verified: whole C++ suite (392 tests). Not run: the Python suite, sanitizers on this change, Windows, macOS.
