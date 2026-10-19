# httpp: Checklist

> ⚠️ **Before starting any phase, the decisions in section 7 of `docs/Architecture.md` that belong to that phase must be made.**
>
> Marks: `[x]` done and verified, `[ ]` not done. Measurement conditions are next to every number.
> **The sandbox that produced the measurements is single-core**, so anything that says "multi-core" has not been measured yet.
> Accepted decisions are in [docs/adr](docs/adr/README.md). What CI covers and which tests are skipped is in [ADR-0015](docs/adr/0015-testing-and-ci.md).

---

## Current status (built and verified so far)

- [x] Own async server engine (Asio + llhttp): keep-alive, pipelining, chunked, limits (413/414/431)
- [x] Server on top of it: router (`:name`, plus `{name}` and `<name>` in the Python layer), static files with HEAD/Range, hooks, `route_async` with a 504 timeout
- [x] Async client engine: TLS (mbedTLS), keep-alive pool, proxy (HTTP and CONNECT), redirects, cookies/auth
- [x] Internal logging (AixLog) with a C++ and a Python API; access log and `listening on ...` line at `info` ([ADR-0011](docs/adr/0011-logging.md)); in Python it goes through `logging` automatically and the `"httpp"` logger level controls the C++ level ([ADR-0018](docs/adr/0018-python-logging-bridge.md))
- [x] `httpp run main:app`, `app.run()`, SIGINT/SIGTERM with a clean shutdown; `httpp run` wins over a `listen` inside the file ([ADR-0012](docs/adr/0012-run-wins-over-listen.md))
- [x] `--workers N` (fresh processes + `SO_REUSEPORT`; Linux only; no respawn)
- [x] Async bridge without the GIL on the C++ side (loop inbox) and the rebuilt hot path ([ADR-0009](docs/adr/0009-hot-path.md))
- [x] FastAPI-like layer: `Query/Path/Header/Cookie/Body/Depends`, OpenAPI 3.1 from the same plan, 422 errors as `{"detail": [...]}` ([ADR-0007](docs/adr/0007-openapi-from-one-plan.md))
- [x] Own Pydantic-v2-shaped models with Cython validators ([ADR-0006](docs/adr/0006-own-model-layer.md))
- [x] Swagger UI compiled into the library, `httpp::mount_swagger_ui` in C++ and `Server.mount_swagger_ui` in Python ([ADR-0008](docs/adr/0008-swagger-ui-compiled-in.md))
- [x] Mbed TLS thread safety (`MBEDTLS_THREADING_C`, alt layer on Windows), ThreadSanitizer clean on the server engine and the TLS client ([ADR-0010](docs/adr/0010-mbedtls-threading.md))
- [x] Wheels: `cp39-abi3` plus `cp313t`/`cp314t`/`cp315t`, built and tested in CI on Linux, macOS and Windows; the Linux wheel is about 1.8 MB ([ADR-0013](docs/adr/0013-packaging.md))
- [x] Free-threaded Python: the GIL stays off after import (checked by a test on the free-threaded wheels in CI)
- [x] CI is green: C++ build on Linux/macOS/Windows, C++ tests on Linux/macOS, Python tests in every wheel
- [x] Tests: 229 C++ tests; 562 Python tests with `pydantic` and `openapi-spec-validator` installed (280 without them, which is what the wheel CI runs), 1 skip that only applies to free-threaded builds. Single core, Linux, CPython 3.12.

---

## Python binding policy (ADR-0017)

> Rules: speed decides where logic lives; Python binds only what `include/` exports; a public C++ function is bound only if it is
> necessary; Python may do extra work that C++ does not need (the GIL, the asyncio bridge). See [ADR-0017](docs/adr/0017-python-binding-policy.md).
> `tests/python/test_cpp_python_parity.py` forces every public C++ name to be either bound or listed in `NOT_EXPOSED` with a reason,
> and checks that the `.pyx` files include only public headers.

### Decide (bindings added before the policy)

- [x] `httpp.utils.sha256_hex`, `sha256_file`, `parse_sha256_checksum`: kept on purpose (binding them lets Python test the C++ code against `hashlib`), but in `httpp.utils`, not in the main namespace
- [ ] `Client(keep_alive=False)`: no other way to turn keep-alive off; waiting for the owner's decision
- [x] `Download.sha256`, `Download.sha256_url`, `Download.set_timeout`, `Download.set_max_wait(seconds)`, `DownloadResult.sha256`: no other way to verify or tune a download from Python

### Duplicated logic reviewed by speed (rule 1)

- [x] Path templates: one parser, `httpp._params.to_cpp_pattern` / `path_names` / `openapi_path` (the second regex in `server.py` is gone); registration time, so not a speed question
- [x] Query and form encoding in `Client`/`AsyncClient`: now the C++ `url::build_query` (1.7 us for 3 pairs against 6.3 us with `urllib`, same output on 3,000 random inputs; a request on loopback is about 107 us)
- [x] The duplicated option block of `Client.__init__` and `_AsyncClientCore.__init__`: one `_apply_client_options` (a Cython fused type over the two C++ clients)
- [ ] MIME types: `serve_directory` (C++ table, deterministic) and `file()` (Python `mimetypes`, which reads the OS tables) disagree for 5 of 33 common extensions (`ico`, `gz`, `wav`, `yaml`, `yml`) and `mimetypes` costs about 3 us per call. The C++ lookup is internal, so using it means exporting a new public function (a decision for the owner)
- [ ] Sync client (`client.cpp`, cpp-httplib) versus async client (`async_http.cpp`): two engines for one behavior; separate decision

### Not exposed in Python (decided, see `NOT_EXPOSED`)

- `route_io` and `route_shared` (Python handlers need the GIL, so `add_loop_route` and `add_route` are the fast path)
- `find_swagger_ui_asset` (internal lookup)
- `httpp::curl::request` is covered by name through `httpp.Request`

### Python only on purpose

- OpenAPI generation, `Query/Path/Header/Cookie/Body/Depends`, models and validation, `on_startup`/`on_shutdown`, `run()` and the `httpp run` CLI

---

## Phase 0: boundary measurement (first of all)

> Goal: find the cost of each Python/C++ crossing **in our own binding**.

### Tests

- [ ] Cost of an empty call from Python into Cython (needs a noop function used only for benchmarks; agree first)
  - [ ] N = 1,000,000
  - [ ] `time.perf_counter` (not cProfile)
- [ ] (optional, only for the paper) compare with ctypes, cffi ABI/API, pybind11
- [ ] Number of crossings per request
  - [ ] C++ atomic counter
  - [ ] separately for each kind:
    - [ ] entering the handler
    - [ ] leaving the handler
    - [ ] state access
    - [ ] parameter validation
    - [x] building the Python object: only what the handler asked for is built (`RawRequest` shared with C++, lazy `ServerRequest`)
- [ ] Boundary share = sum of (cost x count)
- [x] Async: how many hops? **`def`: one hop (Asio to pool); `async def`: one hop (Asio to the asyncio loop) without `before_request`, two with it.** The second hop was removed (option B).

### Output

- [ ] Table of the cost of each kind of crossing
- [ ] Table of crossings per request
- [ ] Chart of each boundary's share

---

## Phase 1: Baseline (single core)

> Goal: prove there is an improvement even on one core.

### Tests

- [x] Simple `def` handler: better than uvicorn? **Yes.** Latest run: 29.5k vs 23.0k requests per second on `hello` (+28%), 25.2k vs 21.1k on `item` (+19%)
- [x] Simple `async def` handler: at least equal? **Yes now.** 33.4k vs 23.0k on `hello` (+45%); on `item` 24.9k vs 21.1k, and `def`/`async def` are within the noise there (about 12%). Before the hot path was rebuilt it was 17.5k vs 24k (-28%), see [ADR-0009](docs/adr/0009-hot-path.md)
- [ ] Constant handler: does Python stay asleep? (🎯 `get_const` not built)
- [ ] Shared memory: measurably better? (not built)
- [ ] Concurrent profiling (perf / py-spy)

> Conditions for the latest numbers: one CPU core shared by the server and `oha`, 32 connections, 2 rounds of 4 seconds, median, uvicorn with httptools and uvloop. They are only comparable with each other.

### Output

- [x] Throughput table (above)
- [ ] Latency table (p50, p95, p99)
- [ ] Profiling flame graph
- [ ] Analysis: why we do or do not improve

---

## Phase 1.5: speed improvements

- [x] Remove one hop for `async def` (route match on the Asio thread when there is no `before_request`)
- [x] Build only the fields the handler needs (and keep the behavior of exception hooks)
- [ ] `get_const` for constant responses
- [ ] `--workers auto` is implemented; check its behavior on free-threaded Python (one process)
- [ ] Before/after measurement for each item with the Phase 1 method, on a multi-core machine (benchmark with `agent` and `many` scenarios, 4 rounds)

---

## Phase 2: multi-core

> Goal: a scaling chart and a breaking point. **Needs a multi-core machine.**

### Axes

- [ ] Cores: 1, 2, 4, 8, 16
- [ ] Handler: sync, async, const, CPU, I/O
- [ ] Workers: 1, 4, 8
- [ ] Modes: `cp39-abi3` single process, the same code on 3.14t, and `--workers N`
- [ ] Speedup chart (not raw throughput)

### Output

- [ ] Scaling chart
- [ ] Breaking point (where does scaling break?)
- [ ] Analysis: why?

---

## Phase 3: comparison

> Goal: separate the share of each boundary.

### Compare with

- [ ] uvicorn + FastAPI
- [ ] uvicorn + plain Starlette
- [ ] `uvicorn --workers N` (a fair comparison with our `--workers N`)
- [ ] Pure C++ server (no Python handler)
- [ ] httpp

### Output

- [ ] Comparison table
- [ ] Analysis: what does each boundary cost?

---

## Phase 4: depth

> Goal: behavior under real conditions.

### Axes

- [ ] TLS: no, yes (**the server has no TLS yet** 🎯; client only)
- [ ] Payload: 100 B, 10 KB, 1 MB
- [ ] Load: closed, open
- [ ] Latency: p50, p95, p99

### Output

- [ ] Full table
- [ ] Analysis: cost of mbedTLS
- [ ] Analysis: backpressure

---

## Phase 4.5: server resource limits (security, risk 7)

- [ ] `set_max_connections` (today only a backoff when fds run out)
- [ ] Header timeout and a limit on slow requests
- [ ] Per-route timeout
- [ ] Slowloris test (an expected failure until these exist; record it in ADR-0015 when added)
- [x] ThreadSanitizer on the server engine and the `io` path: no reports
- [x] ThreadSanitizer on the TLS client: the data race inside mbedtls is fixed ([ADR-0010](docs/adr/0010-mbedtls-threading.md))
- [ ] AddressSanitizer

---

## Phase 5: WebSocket

> ⚠️ Decide backpressure (risk 6) before starting. Not started.

### Steps

- [ ] Handshake (Upgrade)
- [ ] Frame parsing (text)
- [ ] Ping/Pong
- [ ] Close handshake
- [ ] Binary frames
- [ ] Fragmentation
- [ ] wss:// (needs server TLS)
- [ ] Backpressure (second pass)
- [ ] permessage-deflate (optional)

### API

```python
@app.websocket("/ws")
async def ws(websocket):
    await websocket.accept()
    async for msg in websocket:
        await websocket.send(msg)
```

---

## Phase 6: shared memory

> ⚠️ Decide safety (risk 2) and cross-platform (risk 5) before starting. **Linux first.** Not started. The scope of `app.shared` is an open question.

### Steps

- [ ] Choose the implementation (libsharedmemory vendored, or our own behind an interface; risk 2)
- [ ] The supervisor creates the memory before the workers start
- [ ] Thin Python layer (proxy)
- [ ] API: `app.cache` (value: `bytes`; a Python object needs explicit serialization)
  - [ ] `__getitem__`
  - [ ] `__setitem__`
  - [ ] `__delitem__`
  - [ ] `__contains__`
- [ ] API: `app.counter`
  - [ ] `incr()`
  - [ ] `value`
- [ ] Robust inter-process lock (a worker dies while holding it)
- [ ] Cross-platform: Linux first; Windows later
- [ ] Crash recovery
- [ ] Multi-process test (the pattern of `tests/python/test_run.py`)

### Critical tests

- [ ] `test_cache_shared_across_workers`
- [ ] `test_counter_atomic`
- [ ] `test_crash_recovery`
- [ ] `test_shm_linux`
- [ ] `test_shm_windows` (after Linux)

---

## Phase 7: paper

> Goal: turn data into findings.
> **Note:** the data is not uniformly positive (see risk 1 and the `item` scenario). The paper must analyze that honestly,
> not only show the good results. Every number must come from a real run with full conditions.

### Axis

- [ ] Removing the boundary

### Mechanism

- [ ] Profiling
- [ ] Crossing counter
- [ ] Controlled comparison

### Data

- [ ] Multi-core benchmark
- [ ] Comparison table
- [ ] Scaling chart

### Writing

- [ ] Abstract
- [ ] Introduction
- [ ] Related Work
- [ ] Design
- [ ] Evaluation
- [ ] Limitations (single core, Linux, no server TLS)

---

## Phase 8: Starlette/FastAPI-like API layer (native)

> ⚠️ Decide the API scope (risk 4) with the user before starting. **No ASGI adapter is needed.**

### Steps (Starlette level)

- [ ] `Request` / `Response` types with Starlette's interface (`JSONResponse`, `HTMLResponse`, `RedirectResponse`, `PlainTextResponse`, streaming, cookies)
- [ ] `HTTPException` and default exception handlers (today the 422/500 bodies are hand-made); `status` constants
- [ ] `APIRouter` / `Router` / `Mount` / `include_router` (prefix, tags, dependencies)
- [x] `{id}` syntax in paths (Python layer)
- [ ] Middleware (CORS, GZip)
- [ ] Static file mount (the base is in C++)
- [ ] Background tasks (`BackgroundTasks`)
- [ ] `lifespan`
- [ ] WebSocket (depends on phase 5)
- [ ] `httpp.testclient.TestClient` with Starlette's `TestClient` interface, on a real loopback server (today tests use a `running(app)` helper; see `tests/python/test_openapi.py`)

### Steps (FastAPI level, after the decision)

- [x] Path and query parameter types from annotations, with 422 responses
- [x] `Depends`
- [x] OpenAPI and Swagger UI
- [ ] `app.dependency_overrides`
- [ ] `Security()`
- [ ] Forms and file upload (multipart; today the body is JSON only)
- [ ] `response_model_exclude_unset` and similar
- [ ] Differential test against FastAPI (the same app on both, compare responses and `openapi.json`) to find how far "replacement" is true

### Measurement (from a real run)

- [ ] Run Starlette's tests (a fixed tag, cloned in CI) with an import shim and our `TestClient`
- [ ] Record PASS/FAIL/SKIP and the category of each failure
- [ ] Then the same for FastAPI

---

## Server, multi-core and lifecycle

- [ ] `app.shared` (space shared between workers); open question: only a bytes table and counters, or more?
- [ ] Connection limit; `--reload`; respawn of workers
- [ ] Experiment with sub-interpreters (still an open question)
- [ ] Confirm that Cython validation (`freethreading_compatible=True`) scales on free-threaded Python with several cores

---

## Small leftovers and cleanups

- [ ] Race in the `raw_server` test helper (`tests/cpp/test_async_http.cpp`, around lines 54 and 71): the main thread closes the socket in the destructor while the server thread reads it; close it with `asio::post` on `io` or guard it with a mutex
- [ ] `-Woverlength-strings` warning for `httpp_swagger_ui_data.hpp` (multi-megabyte literals); suggestion: make `HTTPP_EMBED_AS_ARRAY` the default
- [ ] Swagger UI body is copied on every request (`ponytail:` in `swagger_ui.cpp`); use a static `string_view` body
- [ ] Evaluate a lighter docs UI (RapiDoc is about a third of Swagger UI, see [ADR-0008](docs/adr/0008-swagger-ui-compiled-in.md)); no decision yet
- [ ] 422 for `int`/`float` path parameters still uses the old `{"error": ...}` body (one existing test depends on it); move everything to `{"detail": [...]}`
- [ ] Cleanup of dependencies with `yield` runs before the response is written (FastAPI does it after); decide whether to match
- [ ] Add a guard (`if __name__ == "__main__":`) to the examples that call `listen` at import time (8 of 15 examples), so they also work with `httpp run`
- [ ] README and `examples/python/` for the new features (OpenAPI, `Depends`, `mount_swagger_ui`)
- [ ] `docs/index.rst` is a Sphinx stub whose `toctree` entries do not exist
- [ ] `CHANGELOG.md` 0.7.0 says "Async HTTP/HTTPS server"; the server has no TLS, check and reword
- [ ] Add `pydantic` and `openapi-spec-validator` to a CI job so the 278 differential tests run there (see [ADR-0015](docs/adr/0015-testing-and-ci.md))
- [ ] Run the C++ tests on Windows in CI (today only the build runs)
- [ ] 405 instead of 404 for a wrong method
- [ ] Symlink protection for static files
- [ ] Example of using the TLS client
- [ ] Stale comments in `progress.hpp` (around lines 16 and 43)
- [ ] `/docs` was only seen in Chromium; check other browsers
