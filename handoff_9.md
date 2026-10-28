# Hand-off 9

State: `io_threads` (ADR-0024) and the benchmark suite are done. C++: 293 tests, TSan clean. Python: 623 tests pass on 3.13, 3.14 and 3.14t.

## Done since hand-off 8
- Inline `async def` (ADR-0023), allocation work, `set_io_threads`, lanes (loop per I/O thread without a GIL), `--io-threads`.
- `benchmarks/Makefile` + `suite.py`: `make bench` writes `benchmarks/results/<time>.txt`.

## Next
0. GIL builds: CPU-bound `def` is still slow on 16 threads (ADR-0025 addendum): pool base is now 4; check `make bench ONLY=cpu`. Free-threaded Python does not have the problem.
1. Read the owner's `make bench` output (16 threads, P-cores 0-11): pick the default `io_threads`, check that `async def` scales on 3.14t,
   that C++ `route_io` scales with threads, that CPU-bound `def` scales on 3.14t. Record the numbers in ADR-0024 (addendum).
2. WebSocket (RFC 6455) in the C++ core, then the Python binding; ADR and tests first.
3. Small items: README gaps (HTTPException, response classes, status, 405), slow first batch of 8 blocking handlers, `listen()` via
   `signal.set_wakeup_fd`, thread-safety review of module-level Python state under free-threading, FastAPI benchmark.

## Not verified
Many-core scaling, Windows, macOS.
