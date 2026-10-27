# Benchmarks

`make bench` (about 15 minutes; `make quick` about 3) builds what it needs, runs everything and prints the path of one result file.
It needs network once (venvs, `oha`) and a C++ compiler for the C++ server. Plug the charger in and close heavy programs first.
`make check` shows what would disturb the numbers. Variables are listed at the top of the `Makefile`
(`SERVER_CPUS`, `LOAD_CPUS`, `IO_THREADS`, `ROUNDS`, `SECONDS`, `CONC`).

`suite.py` is the runner (interleaved rounds, server and load generator pinned to separate CPUs, server CPU per request from `/proc`).
`bench.py`, `io_bench.py`, `cpu_bench.py` are the older single-purpose scripts. Numbers are only comparable inside one result file.
