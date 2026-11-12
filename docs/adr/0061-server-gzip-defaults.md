# ADR-0061: Server gzip stays off by default, level 1, and its cost is documented

- **Status:** accepted (no code change; records `set_compression` and measures it)
- **Decided by:** Claude, on the owner's "continue"
- **Source:** `src/core/response_compression.cpp`, `include/httpp/server.hpp` (`set_compression`), `benchmarks/gzip_bench.py`, `benchmarks/gzip_app.py`

## Context

`set_compression(min_size, level)` (Python `Server(compression=, compression_level=)`) gzips text answers (text, JSON, XML, JavaScript, SVG) of at least `min_size` bytes for clients that send `Accept-Encoding: gzip`. Off by default, level 1 to 9 (zlib's numbering), `Vary: Accept-Encoding` added, incompressible results kept plain, streams, files, ranges and already encoded answers skipped. It had no ADR and no numbers.

## Measurement

Sandbox, 2 cores, Python 3.14t, one I/O thread, server pinned to CPU 0 and `oha` (32 connections) to CPU 1, loopback, median of 3 rounds of 3 s, JSON list of small records. Loopback means only the CPU cost shows, not the saved bandwidth. "cost" is the throughput of the plain answer divided by the throughput with gzip.

| answer | level | req/s | bytes on the wire | cost |
|---|---|---|---|---|
| 1 KB | off / 1 / 6 / 9 | 81.9k / 24.9k / 23.5k / 23.5k | 1073 / 351 / 335 / 332 | 1x / 3.3x / 3.5x / 3.5x |
| 10 KB | off / 1 / 6 / 9 | 62.9k / 9.3k / 7.8k / 3.8k | 10351 / 2187 / 1865 / 1768 | 1x / 6.7x / 8.0x / 16.4x |
| 100 KB | off / 1 / 6 / 9 | 18.2k / 1.09k / 0.62k / 0.12k | 102488 / 19499 / 15692 / 14470 | 1x / 16.7x / 29.3x / 150x |

The same bodies through zlib in Python (`zlib.compress`, one run of 0.5 s each): 100 KB gives 21444 / 15809 / 14612 bytes in 0.45 / 1.34 / 7.0 ms at levels 1 / 6 / 9; 10 KB gives 2305 / 1895 / 1779 bytes in 31 / 79 / 205 us. So the vendored codec compresses level 1 better than zlib's level 1 (19.5 KB against 21.4 KB) but takes about two to three times as long there (about 0.87 ms against 0.45 ms for 100 KB, subtracting the plain request cost); at levels 6 and 9 it is within 15 to 40 percent of zlib. This subtraction is an estimate, not a direct timing.

## Decision

No code change. Off by default (a server behind a compressing proxy must not compress twice, and the cost above is large for small, hot answers), level 1 stays the default (smallest cost; levels 6 and 9 save a further 15 and 25 percent of the bytes at 2 to 9 times the time, not worth it as a default), `min_size` stays the owner's choice. The header comment in `server.hpp` already says to use a larger `min_size` or a lower level when answers are big and latency matters; this ADR gives the numbers behind that.

## Consequences

- Turning gzip on costs a 3 to 17 times lower request rate for 1 to 100 KB JSON on one core. It pays only where the network, not the CPU, is the bottleneck, which loopback cannot show.
- The compression runs inside the response building of the request, after the hooks (`server.cpp`, `c->compression.apply`); I did not check for every route kind which thread that is, so do not assume it leaves the I/O thread free.
- Possible follow-up, not done: a faster level 1 or a size-based level (for example level 1 above 64 KB only), caching of compressed static answers.

## Verification

`benchmarks/gzip_bench.py` run in the sandbox. Not verified: a real network (bandwidth saved against CPU spent), more than one core for the server, other content (HTML, mostly repeated text compress differently), which thread compresses for `async def` and `route_io` routes, comparison with a real zlib inside the server.
