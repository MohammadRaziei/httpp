# httpp hand-off (consolidated, 2026-11-05)

This one file replaces every earlier hand-off. Read it first; the ADRs in `docs/adr/` hold the reasoning and `checklist.md` holds the open work.
Owner: Mohammad (GitHub `mohammadraziei`), the author of httpp. Section 0 is how we work; sections 1 to 4 are what exists; section 5 is the state of the last session; section 6 is the next work.

## 0. How the owner and I work together (read this first)
This is a long collaboration, not a one-off task. Mohammad is an experienced systems developer who owns the code and the decisions about his goals; I am the hands that read, measure, write, test and explain.
- **Language and tone.** Conversation in Persian, plain words, short paragraphs; a table or a number is fine, but he says "I don't understand" when it is terse, so explain what it means. Everything inside the repo is English, no em dash, no Persian anywhere in code, comments, tests, docs or commands.
- **He delegates, I decide and record.** For performance and architecture he says "continue" or "you decide": then I pick the most principled option, say what and why in a few lines, and record it in an ADR (check `docs/adr/` for the next free number first; numbers have raced with upstream; the next free one is 0037). Only constraint: the C++ and Python APIs stay dual. "Continue" means: take the proposed plan, decide the open questions, and go.
- **Never push, never commit.** He commits and pushes. After every step build `httpp.zip` (`cd <parent> && zip -qr httpp.zip httpp -x '*/__pycache__/*' '*/build/*' '*/.git/*' '*/.venvs/*' '*/.build/*' '*/bin/*'`; delete any `venv*` folder first) and send it with SendUserFile without asking.
- **Honesty is the rule he checks hardest.** State what was measured and what was not; never claim parity or a lead over a competitor from a single or unfair run; own mistakes (this session: I wrote a perf filter that returned nothing, and described `diag_io.sh` before reading that it dropped `keep_alive_max`; both were said out loud and fixed). Keep a "Not verified" list. Numbers compare only inside one interleaved run on his machine. A hypothesis is called a hypothesis.
- **He runs things I cannot.** The sandbox has 2 cores, no `perf`, no `oha`, a proxy allowlist (PyPI, GitHub, crates.io work) and no Windows or macOS. For anything that needs his machine I give one copy-paste block, run from the project root `/mnt/windowsD/Projects/httpp` (an NTFS drive mounted under Linux; he is on Ubuntu with zsh, so quote globs and keep loops in plain bash style), tell him what to send back (usually a small zip without raw `.data` files), and then read the files. He pastes terminal output or sends zips. Do not make him guess which directory or which flags. When a result file arrives, open it and check it really holds what was asked (this session two of three report files were empty because of a wrong filter).
- **He pushes back and thinks aloud.** He proposes ideas (arena allocator, SIMD in llhttp, copying Drogon's parser, own parser, a C++ CLI). Answer with the numbers we have, the risks (safety, portability, vendor rules), and a cheaper alternative; then he decides. When he decides ("forget it, llhttp stays"), record it and move on without re-arguing.
- **Small wins add up.** He values several 1 to 3 percent gains together, but every change is checked: C++ suite, ThreadSanitizer for anything concurrent, ASan for lifetime changes, and a test that a request outlives its handler when touching the request type.
- **Process.** Task list for multi-step work; tests before code (TDD); `ponytail:` comment on each deliberate simplification with its upgrade path; zero system dependencies; STL-like snake_case; macOS 10.13 limits in `src/core` (no `std::filesystem` there; the Windows branch may use it).
- **Permissions.** If the sandbox safety classifier refuses something, explain and ask; do not work around it.
- **Sandbox shell traps found this session.** `pkill -f <pattern>` can match the tool's own shell and kill it (use a captured PID). Background jobs started with `nohup` or by a non-interactive script ignore SIGINT, so a valgrind run could not be stopped: drive such runs from Python `subprocess` (see "Recipes"). A `timeout` wrapper has the same effect.

## 1. What httpp is
A C++ HTTP library (async server on Asio + llhttp, clients, downloads, progress bars, URL, WebSocket, read-only file browser) with a Cython/Python layer. Direction (ADR-0001): httpp replaces FastAPI/Starlette above and uvicorn below: a Starlette/FastAPI-like API implemented natively on our own engine, launched by `httpp run main:app` or `app.run()`. Handlers run on a standard asyncio loop (ADR-0002); no ASGI adapter is needed (low priority). One design for the C++ and the Python API; only GIL related differences.

## 2. Standing rules
- `cpp-httplib` (`src/third_party/cpp-httplib/httplib.h`, v0.54.1) stays byte-identical to upstream; llhttp is unmodified and stays vendored (owner decision, 2026-11-05: no SIMD patch, no own parser, no copy of Drogon's parser; do not bring it up unless a profile shows the parser is the hot path and he asks). Document any vendor change in `src/third_party/README.md`. Asio, mbedtls, liburlparser are submodules.
- Zero system dependencies (nothing from apt/brew; vendor or submodule; pip build deps are fine).
- macOS wheels target 10.13: avoid `std::filesystem`, floating `to_chars`, `std::optional` accessors and other post-10.13 libc++ APIs in `src/core`.
- Be honest about "async" and the GIL: say what is truly non-blocking and what is a thread pool.
- Do NOT make `request` hold `string_view`s into the connection's read buffer: `route_io` and `route_shared` requests outlive the handler (tests `the_request_outlives_the_handler...`, ADR-0027) and the buffer is reused. Safe variants: `reserve` plus move, or one buffer owned by the `request` itself.

## 3. Architecture today
```
include/httpp/        public API (client, server, websocket, download, url, progress, router, file_browser, swagger_ui ...)
src/core/             async_server.cpp (Asio engine + llhttp; tunnel for upgrades), server.cpp (router, hooks, static files, run_ws),
                      websocket.cpp + internal/websocket.hpp, websocket_api.cpp, async_http.cpp (async client), client.cpp / download.cpp /
                      curl.cpp (httplib, sync), file_browser.cpp, inflate/deflate/content_decode (vendored stb codecs), log.cpp, tls_session.cpp
src/assets/           file_browser.html (embedded with COMPRESS)
src/bindings/python/  httpp_cy.pyx (Cython glue), httpp/server.py, _params.py, _websocket.py, _run.py, _validate*.py, _router.py, __main__.py ...
tests/cpp  tests/python  benchmarks/  tools/autobahn/  docs/adr/  docs/Architecture.md  checklist.md
```
Request paths:
- **`def` route**: I/O thread (parse + match) -> C++ worker pool thread -> takes the GIL -> handler. Pool starts at 1 thread with a GIL and grows after a 10 ms wait (ADR-0025).
- **`async def` route**: I/O thread -> `route_io` -> per-lane inbox (mutex + deque, wake byte; no GIL on the I/O thread) -> asyncio loop thread of that lane drains a burst with the GIL held once -> task -> `done()`. A handler that cannot suspend may run inline on the I/O thread when its loop sleeps (ADR-0023); default on only with 2 CPUs or fewer.
- **Lanes (ADR-0024)**: `set_io_threads(n)` = one engine per I/O thread on one port (SO_REUSEPORT). Python: one asyncio loop per I/O thread only on a free-threaded build, else one loop. Default `io_threads` = `min(4, CPUs // 4)` with a GIL, 1 without a GIL or on Windows. With several I/O threads a new connection goes to the engine with the fewest connections (ADR-0034).
- `after_request`/`error_handler` hooks make `done` go through `run_in_executor`. The hook path of `make_sender` no longer touches `this` after `weak.lock()` (ADR-0035).
- **WebSocket (ADR-0026, 0027)**: generic engine tunnel; `ws_session` implements RFC 6455 (frames, fragments, control frames, close handshake, UTF-8 and close-code checks, bounded send queue, ping). Python `@app.websocket(path, ...)` with a Starlette-shaped `websocket` object, per-connection C++ mailbox. Same-origin default policy. Not supported on WebSocket routes: dependencies, `response`, background tasks, OpenAPI, `APIRouter.websocket`. No permessage-deflate, no TLS of its own.
- **File browser (ADR-0032)**: `httpp::mount_file_browser(server&, base, root, file_browser_options)`; Python `Server.mount_file_browser(base, root, **options)`; CLI `httpp browse [DIRECTORY]`. Read-only; the root is a ceiling (`..`, absolute paths, backslash, NUL, symlinks out are refused with the same 404); dotfiles hidden unless `show_hidden`; active types always downloads; no login (use the `before` hook); `search` filters the open folder; no ETag in v1.
- **Server compression**: `set_compression(min_size, level)` / `Server(compression=...)`: text answers only, off by default; no ADR yet and no benchmark.

## 4. Build and test
- Sandbox: 2 cores. Venvs via `uv`: `uv venv --python 3.14 venv314` (also `3.14t`, `3.13`); install build deps (`cmake ninja cython jsonschema jinja2 cython-cmake pytest pytest-timeout scikit-build-core pydantic`); install the project with `VIRTUAL_ENV=<venv> uv pip install --python <venv>/bin/python ./httpp` (60 to 100 s; a `.pyx` change needs it). Python tests: `<venv>/bin/python -m pytest httpp/tests/python --timeout=120`. A failed connection from `httpp.get` is a response with status 0, not an exception.
- C++ tests: `cmake -S . -B ../build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHTTPP_BUILD_PYTHON=OFF -DHTTPP_BUILD_TESTS=ON`; build `--target test_httpp_cpp` (the default target fails in the nested Python build); run `tests/cpp/test_httpp_cpp --filter='httpp_ws*'`. For a library only: `--target httpp_core`.
- Sanitizers: separate build dirs (`../build-asan`, `../build-tsan`) with `-fsanitize=address|thread -fno-omit-frame-pointer -g -O1` in `CMAKE_CXX_FLAGS`, `CMAKE_C_FLAGS`, `CMAKE_SHARED_LINKER_FLAGS`, `CMAKE_EXE_LINKER_FLAGS`, `-DHTTPP_BUILD_PYTHON=OFF -DHTTPP_BUILD_TESTS=ON`. Each takes about 7 minutes to build here; start it detached and poll. Do not configure two build dirs at once (the liburlparser configure rewrites a tracked `public_suffix_list.dat` and needs the network).
- Benchmarks (ADR-0030): `cmake -S benchmarks -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release`, then `--target bench_quick` or `--target bench`; settings are cache variables (`BENCH_ONLY`, `BENCH_SECONDS`, `BENCH_ROUNDS`, `BENCH_IO_THREADS`, `BENCH_PYS`). The `bench` target builds virtualenvs with `uv`, which currently cannot reach PyPI from the owner's network (about 1.6 KB/s, then timeout), so the official bench has not run since ADR-0034. Owner's machine: Ubuntu 26.04, i7-13620H, 16 threads (P-cores 0-11, E-cores 12-15), core-aligned split (server CPUs 0-7, load generator 8-15), governor `powersave` (wants `performance`).
- `benchmarks/diag_io.sh [IO_THREADS]` needs no pip: perf, per-thread CPU ticks, counters per request, for `cpp_bench` or (`BENCH_CPP_BIN=build-bench/drogon_bench BENCH_DIAG_PATH=/sync`) Drogon. It now starts the server with `keep_alive_max` 0 (`BENCH_KEEP_ALIVE_MAX` overrides). Its first number is measured under `perf record`.

### Recipes that worked in the sandbox
- Instructions per request: compile `benchmarks/cpp/cpp_bench.cpp` against `build/libhttpp_core.so` (`g++ -O2 -g -std=c++17 -Iinclude -I../build ... -lhttpp_core -Wl,-rpath,...`), run it under `valgrind --tool=callgrind` with `keep_alive_max` 0 and 1 I/O thread, drive 2000 and then 22000 requests of `/io` over 8 keep-alive connections with a small Python socket client, take (Ir 22000 minus Ir 2000) / 20000. Start and stop valgrind from a Python script with `subprocess.Popen` and `send_signal(SIGINT)` (a shell script cannot stop it). Call counts per request: parse `calls=` lines of the callgrind files and divide the difference by 20000. `callgrind_annotate --tree=caller` shows callers; calls through the PLT show up as bare addresses, so map them by their call counts.
- `perf` on his machine: his `perf report --symbol-filter=...` did NOT filter (all outputs were identical); use `--sort dso,symbol` plus `grep`, or `-S` with the mangled name.

## 5. State after the last session (2026-11-05)
Nothing is committed or pushed (the owner commits). Version in the tree is 0.8.0; `CHANGELOG.md` has the pending items under `[Unreleased]` (next version 0.8.1).

### Verified
- C++ suite 407 tests pass in Release, and are clean under AddressSanitizer (no memory errors) and ThreadSanitizer (no warnings).
- Python suite: 3.14 with the GIL 665 passed, 4 skipped; 3.14t 664 passed, 5 skipped; 3.13 665 passed, 4 skipped (all after the file browser and the `string_view` change).
- Autobahn (owner, CI, earlier): 589 OK, no FAILED.

### What changed in this and the previous session (all uncommitted)
- ADR-0033 items 9 to 12 and the 2026-11-05 addendum (literal comparisons through `std::string_view`: `strlen` calls per request 10 to 7, instructions per request 13440 to 13327, about 0.8 percent, callgrind in the sandbox).
- ADR-0034 balanced connections; ADR-0035 hook path without `this`; ADR-0036 read buffer not zero-filled.
- File browser: C++ core (15 tests), Python wrapper `Server.mount_file_browser`, command `httpp browse`, `tests/python/test_file_browser.py` (12 tests), README section, parity test entry, ADR-0032 accepted.
- `benchmarks/diag_io.sh` passes `keep_alive_max` (default 0).

### Performance against Drogon (hello `/io` or `/sync`, 4 I/O threads, owner's machine, `diag_io.sh`, alternating)
- httpp 694k, 693k, 647k, 570k, 657k, 641k (median 652k); Drogon 606k, 597k, 646k, 668k, 603k, 629k (median 618k). The ranges overlap, so the data say "not behind", not "ahead"; the earlier single runs showed a 7 to 10 percent gap, which is gone here. Only the second session used `keep_alive_max` 0.
- Counters of the last pair (single window): cycles per request 23.2k against 23.6k, instructions 28.7k against 30.6k, cache misses 2.6 against 4.5, thread ticks 308 against 300. Drogon's own instructions moved by 3.5k between sessions, so cross-session comparisons of these counters are weak.
- Profile facts (owner's perf, single runs): kernel is 57 to 59 percent of cycles for both servers; per request httpp makes 13 `malloc`/`free`, about 14 `shared_ptr` releases, 3.2 Asio scheduler mutex locks (inside `asio::scheduler::run`), 4 `steady_clock::now`. `malloc` plus `free` is about 3 percent of cycles, string code about 2.7, `_Sp_counted_base::_M_release` 1.96 self. No single call site dominates; headers are already moved. Fewer `shared_ptr` copies are worth at most about 2 percent and touch request lifetime (needs ASan, TSan, the outlive tests); not done.
- Optimization flags are not the gap: httpp builds with `-O3` and LTO (`cmake/Optimize.cmake`); `-march=native` only with `ENABLE_NATIVE` (its default was not checked); Drogon in the bench builds with plain Release flags and no LTO.
- Decision of the owner: this is good enough, roughly equal; stop the Drogon chase and go to features.

### Not verified (keep stating)
Windows and macOS runtime (the Windows branch of `file_browser.cpp` was never compiled), the official interleaved bench on the current tree (network), a `performance` governor, more than one machine, sanitizers on the Python extension, the WebSocket benchmark and idle memory per connection (httpp about 20 KiB against Drogon about 9 KiB in the sandbox, unchecked on his machine), whether the old Date race caused the lost servers, behaviour behind a TLS proxy (`wss`), Python 3.15t locally, the free-threaded Python glue under a long stress.

### Known small findings
- ASan leak report at exit (about 18 KB in 7 allocations, all in `async_http::impl` exchange objects alive at exit): not from the file browser, not investigated.
- ADR-0035 open point: `worker_pool*` itself can be read just before the pool is destroyed.
- The 504 deadline of an unanswered async request starts when its handler returns.
- `async def INLINE` at one I/O thread is slower than the worker-pool path (unexplained); `async def` on 3.14 with a GIL drops from 2 to 4 I/O threads (GIL contention, expected).

## 6. Next work, in order
Full list with checkboxes is in `checklist.md`. The owner asked for these next:

### 6.1 Move the command line into C++ (owner's idea, design decided on his "go")
Goal: `src/cli/httpp_cli.cpp` with the vendored single header `CLI11.hpp`; the executable's output name is `httpp`; Python only starts it: `httpp/__main__.py` passes the right argv to the binary with `subprocess.run` and returns its exit code.
- CLI11 v2.5.0 single header: `https://github.com/CLIUtils/CLI11/releases/download/v2.5.0/CLI11.hpp` (reachable from the sandbox, 444 KB, BSD-3-Clause). Put it in `src/third_party/CLI11/CLI11.hpp` and list it in `src/third_party/README.md` (unmodified, version, license).
- CMake: `add_executable(httpp_cli src/cli/httpp_cli.cpp)`, `set_target_properties(httpp_cli PROPERTIES OUTPUT_NAME httpp)`, link `httpp_core`; install it in the SDK (so `httpp install` and a C++-only install get a real `httpp`) and into the wheel inside the package folder (for example `httpp/_bin/httpp`), not onto PATH, so there is one `httpp` on PATH: the Python console script `httpp = httpp.__main__:main` stays as the shim.
- Commands that need no Python move wholly to C++ and then run with no GIL and no interpreter start: `server`, `browse`, `download`, `curl`. `run` cannot (it imports the user's app): the C++ parser accepts `run` with all its options (so `--help` and validation live in one place) and then re-executes `$HTTPP_PYTHON -m httpp._run_cli <args>`; `__main__.py` sets `HTTPP_PYTHON=sys.executable`. A standalone C++ binary without that variable answers "run needs Python: use `python -m httpp run`".
- Open for the owner: `path`, `install`, `uninstall` (they copy files of the installed wheel): proposed to stay in Python because they must know the wheel layout, which the shim knows for free; the owner said `__main__.py` only forwards argv, so if he wants them in C++ the binary finds its siblings relative to its own path.
- Costs to say out loud: one extra process start (about tens of milliseconds) for every CLI call; the wheel gets a platform executable per wheel (Windows `.exe`, macOS 10.13 limits apply to `src/cli` as well, so POSIX calls instead of `std::filesystem`); `scikit-build-core` must install the executable into the package (component `httpp_wheel`).
- Order: tests first (`tests/python/test_cli.py` already covers `browse`; add `server`, `download`, `curl`, `--help`, exit codes, `run` delegation, the shim passing argv unchanged with spaces and quotes), then the binary, then `__main__.py`, then ADR-0037, CHANGELOG and README. Keep behaviour identical to today's `__main__.py` (flags, defaults, messages) so the existing tests stay green.

### 6.2 Features and polish
See `checklist.md`, sections "Features" and "Quality and release". Highlights: CI green on three OS, a test client, CORS and similar middleware, a TLS server, `APIRouter.websocket`, README gaps, server gzip ADR and benchmark, the official bench when the network works.

### 6.3 Performance leftovers (optional, small)
A ten-round A/B script with medians for both servers; `performance` governor; the official bench; maybe fewer `shared_ptr` copies; reading trantor's connection and buffer handling if a gap shows up again.
