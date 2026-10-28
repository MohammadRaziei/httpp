# Benchmarks

`make bench` (about 15 minutes; `make quick` about 3) builds what it needs, runs everything and prints the path of one result file.
It needs network once (venvs, `oha`) and a C++ compiler for the C++ server. Plug the charger in and close heavy programs first.
`make check` shows what would disturb the numbers. Variables are listed at the top of the `Makefile`
(`SERVER_CPUS`, `LOAD_CPUS`, `IO_THREADS`, `ROUNDS`, `SECONDS`, `CONC`).

`suite.py` is the runner (interleaved rounds, server and load generator pinned to separate CPUs, server CPU per request from `/proc`).
`bench.py`, `io_bench.py`, `cpu_bench.py` are the older single-purpose scripts. Numbers are only comparable inside one result file.

What a result file holds: hello (constant answer), json (a path parameter and a JSON object), cpu (about 60 us of Python or C++ work) and one-connection latency,
for uvicorn (bare ASGI), FastAPI + uvicorn, httpp `def` / `async def` / `await` / inline and the C++ API at 1, 2, 4 and 8 I/O threads; the standard deviation of the
rounds; and latency (p50, p99, p99.9) at half of each variant's own maximum rate, which is the number to read for tail latency (at saturation a closed loop
only measures queueing). The load generator gets the rest of the performance cores and all efficiency cores, because above 300k requests per second it is
the load generator that runs out first: read the C++ and Python rows at 4 and 8 threads with that in mind.
