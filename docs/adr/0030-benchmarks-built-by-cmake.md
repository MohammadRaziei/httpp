# ADR-0030: The benchmarks are a CMake project; WebSocket and Drogon join the suite

## Status
Accepted (2026-10-31). The owner asked for a WebSocket benchmark, Drogon fetched by CMake, and CMake targets instead of `make bench`.

## Context
`benchmarks/Makefile` built the venvs, `oha` and one C++ server by shell commands, with a hand-written `g++` line for the server. It could not
build more than one C++ program cleanly, and there was no WebSocket load generator (`oha` does not speak WebSocket), no memory measurement for many
idle connections (a reviewer asked) and no other C++ framework to compare with. HANDOFF noted that Drogon "needs system packages".

## Decision
- `benchmarks/CMakeLists.txt` is a standalone project (`cmake -S benchmarks -B build-bench`) that adds the library with `add_subdirectory` and
  defines the targets `cpp_bench`, `ws_load`, `drogon_bench`, `bench_env`, `bench_check`, `bench`, `bench_quick`. Settings are cache variables
  (`BENCH_SECONDS`, `BENCH_ROUNDS`, `BENCH_ONLY`, ...). The Makefile is removed. Python-side set-up moved to `bench_setup.py`.
- **Drogon** (v1.9.14) and its jsoncpp are fetched by `FetchContent` and built static, with the options that switch off its ORM, CTL, examples, Brotli and
  YAML. The one system package left is the libuuid headers (Drogon requires them): without them the target is skipped with a message, and the result file
  says "not built". `-DHTTPP_BENCH_DROGON=OFF` removes it.
- **`ws_load`** is our own small Asio program (the vendored Asio, no new dependency): a closed loop of masked text messages with latency percentiles, and an
  idle mode that opens N connections and holds them for the suite to read the server's RSS. Servers compared: the httpp C++ API, Drogon, uvicorn (its websockets
  implementation), the `websockets` library (on uvloop when installed) and httpp `async def`. Compression and keep-alive pings are off everywhere.
- `suite.py` gets the groups `websocket echo ...` and `memory with idle WebSocket connections` (`BENCH_ONLY=ws,idle`), and Drogon rows in the C++ hello/cpu groups.

## Rejected alternatives
- `uWebSockets`, `Crow`, `libhv` and the others from the HANDOFF list: not asked for now; the same `FetchContent` pattern adds them.
- A Python load generator for WebSocket: one Python process cannot generate 100k messages per second; it would measure itself.
- A shim for libuuid or Boost.UUID to drop the last system package: more code for a bench-only dependency. Upgrade path noted in `CMakeLists.txt`.
- Keeping a Makefile that calls CMake: two entry points for one job.

## Consequences
- `make bench` becomes `cmake --build build-bench --target bench`; the old ADRs and CHANGELOG lines that say `make bench` are history and were not edited.
- The first configure downloads Drogon and jsoncpp (GitHub) and building Drogon takes a few minutes.
- **Not verified:** numbers on the owner's machine (the sandbox has 2 cores shared with the load generator, so its WebSocket numbers say nothing about scaling);
  that Drogon builds with other compilers or on macOS; the fairness of the `websockets` library row (its default is a pure asyncio server).

## Related
ADR-0026, ADR-0027, ADR-0028.
