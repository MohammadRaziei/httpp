# Benchmarks

One command for the whole comparison (CMake 3.28 or newer, a C++ compiler, Python 3, network once):

    cmake -S benchmarks -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build-bench --target bench          # about 25 minutes; --target bench_quick is about 5

It builds what it needs, runs everything and prints the path of one result file (`build-bench/results/<time>.txt`). Plug the charger in and
close heavy programs first.

| target | what it does |
|---|---|
| `bench_env` | Python 3.14 (GIL) and 3.14t (free-threaded) virtualenvs with this httpp, uvicorn, fastapi, websockets, and the load generator `oha` (via `uv`) |
| `cpp_bench` | the httpp C++ server of the suite (HTTP routes and a WebSocket echo) |
| `ws_load` | the WebSocket load generator (`oha` does not speak WebSocket); also holds idle connections for the memory table |
| `drogon_bench` | Drogon, fetched and built from source by `CMakeLists.txt` (`HTTPP_BENCH_DROGON`, on by default; needs the libuuid headers, `uuid-dev`) |
| `bench_check` | prints what would disturb the numbers (governor, battery, turbo) |
| `bench` / `bench_quick` | everything above, then the measurement (`bench_quick`: 3 s, one round, 1 and 4 I/O threads) |

Settings are cache variables, set at configure time: `-DBENCH_SECONDS=10 -DBENCH_ROUNDS=5 -DBENCH_IO_THREADS=1,2,4,8 -DBENCH_PYS="3.14 3.14t"
-DBENCH_ONLY=ws,idle -DBENCH_WS_SIZE=1024 -DBENCH_IDLE_CONNS=10000 -DBENCH_SERVER_CPUS=0-7 -DBENCH_LOAD_CPUS=8-15`
(the list is at the top of `CMakeLists.txt`). `BENCH_ONLY` picks parts: `hello json cpu latency ws idle`.

`suite.py` is the runner (interleaved rounds, server and load generator pinned to separate CPUs, server CPU per request from `/proc`);
`bench_setup.py` makes the virtualenvs and checks the machine. `bench.py`, `io_bench.py`, `cpu_bench.py` are the older single-purpose scripts.
Numbers are only comparable inside one result file.

**JSON and the docs page** (ADR-0063). Every run writes `benchmarks/results/<time>[-<machine>].json` next to the `.txt`: the machine, the software, every variant with
its rounds, the fixed-rate latency and the idle memory. `docs/site` reads the committed `.json` files marked `"purpose": "published"` (`python3 docs/site/scripts/prepare.py bench`
does it, `npm start` and `npm run build` too) and draws them on the Benchmarks page. A run for testing the machinery uses `-DBENCH_PURPOSE=smoke`: its file is named `smoke-...`, git ignores it and the site skips it.
`-DBENCH_MACHINE="my laptop"` puts a name on the page.

Servers and clients in the suite: uvicorn (bare ASGI), FastAPI + uvicorn, aiohttp, httpp (`def`, `async def`, C++), Drogon; and, as HTTP clients, httpp `Client` / `AsyncClient`, requests,
httpx (sync and async) and aiohttp (`BENCH_ONLY=client`, `BENCH_CLIENT_CONC=32`). Runtimes: the C++ binaries, Python 3.14 and 3.14t (`BENCH_PYS`). A library with no wheel for an interpreter
is left out there; `meta.pythons[].gil_after_import` in the JSON says when importing a library turned the GIL back on.

`diag_io.sh [IO_THREADS [SERVER_CPUS [LOAD_CPUS]]]` is for one question: why the C++ server does not scale like Drogon with several I/O threads. It runs `cpp_bench` `/io` under
`perf` with kernel samples and counts the CPU ticks of each server thread (are the connections spread evenly?), writes small files into `build-bench/diag/` and prints their paths.
Linux only; needs `perf`, `taskset`, `oha` and `cmake --build build-bench --target cpp_bench`. For Drogon: `BENCH_CPP_BIN=build-bench/drogon_bench BENCH_DIAG_PATH=/sync benchmarks/diag_io.sh`.

What a result file holds:
- **hello** (constant answer), **json** (a path parameter and a JSON object), **cpu** (about 60 us of Python or C++ work) and one-connection latency, for uvicorn
  (bare ASGI), FastAPI + uvicorn, httpp `def` / `async def` / `await` / inline, the httpp C++ API and Drogon, at 1, 2, 4 and 8 I/O threads;
  the standard deviation of the rounds; and latency (p50, p99, p99.9) at half of each variant's own maximum rate, which is the number to read for tail
  latency (at saturation a closed loop only measures queueing).
- **websocket echo**: messages per second and latency of a closed loop of text messages (64 bytes, 64 connections) against the httpp C++ API, Drogon,
  uvicorn (its websockets implementation), the `websockets` library and httpp `async def`.
- **memory with idle WebSocket connections**: the server's RSS before and after N connections were opened and left alone (one run, not a median).

The load generators get the rest of the performance cores and all efficiency cores, because above 300k requests per second it is the load generator that
runs out first: read the C++ and Python rows at 4 and 8 threads with that in mind.
