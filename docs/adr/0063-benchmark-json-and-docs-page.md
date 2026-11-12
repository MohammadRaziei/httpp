# ADR-0063: The benchmark suite writes JSON; the docs site draws the charts

- **Status:** proposed (end-to-end tested once on a one-CPU sandbox, smoke numbers only; waiting for a real run on the owner's machine)
- **Decided by:** the project owner ("a benchmark page for the docs: a script runs on my machine, writes JSON files, Docusaurus reads them and draws them with Chart.js"; "compare with fastapi, uvicorn, drogon, requests, aiohttp and httpx on C++, 3.14 and 3.14t")
- **Source:** `benchmarks/suite.py`, `benchmarks/client_worker.py`, `benchmarks/suite_aiohttp.py`, `docs/site/scripts/prepare.py`, `docs/site/src/components/Bench.tsx`, ADR-0030, ADR-0060

## Context

`benchmarks/suite.py` (ADR-0030) printed a text report that was sent back by hand. The docs site (ADR-0060) had no place for numbers, and `prepare.py` said so ("no committed benchmark JSON"). The suite also lacked aiohttp as a server and had no measurement of HTTP clients at all.

## Decision

- `suite.py` writes a directory of JSON files (schema 1) with fixed names, so that the docs site can load them: `results/full/` or `results/quick/`. A run overwrites the previous one of its kind; the date is inside and git keeps the history.
  - `system.json`: the date, `hardware` (CPU model, cores, clock, caches, memory, OS, kernel, virtualization, laptop or not, battery), CPU pinning, governor, source tree hash, compiler, settings, and the list of the runtimes.
  - one file per runtime, named by its id: `cpp.json`, `py314.json`, `py314t.json` (one per interpreter given with `--pys`). Each holds `results` (one row per variant: suite, server family, I/O threads, median, sd, min, max, percentiles, CPU per request, every round), `rated` (latency at half the maximum rate) and `idle` (memory with idle WebSockets); a Python file also holds the version of the interpreter, whether the GIL is on, the version of each library and whether importing it turned the GIL back on. The HTTP client rows of a Python are in its file, the server they talk to is the C++ one.
- The CMake `bench` and `bench_quick` targets pass the settings and write into `benchmarks/results/` (`BENCH_RESULTS_DIR`). `report.txt` and `results/quick/` are git-ignored; `results/full/` of a real run is committed.
- `--purpose smoke` marks a test of the machinery. `docs/site/scripts/prepare.py bench` puts `results/full/` (schema 1) together as one run in `src/generated/bench.json` (`results/quick/` too when `HTTPP_BENCH_SMOKE=1`), so numbers from a sandbox cannot reach the site.
- New servers: aiohttp (`suite_aiohttp.py`: hello, cpu, json, WebSocket echo; uvloop when installed, and the variant says so). New part `client`: httpp `Client` and `AsyncClient`, requests, httpx (sync and async) and aiohttp make GET requests against one server (the C++ one when built) from a process of their own (`client_worker.py`); sync libraries get one thread and session each, async libraries one session and one task per connection.
- The page `docs/site/docs/benchmarks.mdx` uses `Bench.tsx` (chart.js, react-chartjs-2): one chart per part with filters for runtime, I/O threads and metric, the machine table, a numbers table under each chart.

## Rejected alternatives

- **A new `.sh` entry point.** CMake already builds everything the suite needs and has the settings (ADR-0030).
- **The docs site reading `build-bench/results/`.** It would tie the site build to a local build tree; committed files under `benchmarks/results/` keep the page reproducible in CI.
- **Another chart library or a hand-made SVG.** chart.js was asked for.

## Consequences

- Two npm packages (`chart.js`, `react-chartjs-2`) join the docs toolchain, not the library.
- A package without a wheel for an interpreter (aiohttp on a free-threaded build, say) is left out of that interpreter's rows; `bench_setup.py` says so.
- The client part measures one request shape (GET with a small body) against a local server; it says nothing about TLS, large bodies or a real network.
- Not verified: the numbers on a real machine, the C++ and Drogon rows in this sandbox (see the tests below), Windows and macOS.

## Related

ADR-0030, ADR-0060.
