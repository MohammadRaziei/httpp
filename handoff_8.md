# httpp hand-off 8

Read `handoff_7.md` for the working rules (they still apply), then `docs/adr/0023-inline-async-handlers.md`.

## Done in this session
- Confirmed the batching change: C++ suite 287 of 287 (also under TSan, no report), Python suite 617 passed and 4 skipped (3.13).
- Hop removal for `async def` handlers that cannot suspend (ADR-0023): `_GatedSelector`, `_cannot_suspend`, `_dispatch_hybrid`, `busy` count.
  Tests: `tests/python/test_inline_async.py` (9). One old test was changed (`test_async_handlers_run_on_the_loop_thread...`: its handler now awaits).
- Numbers (interleaved, 2 cores): no-await `async def` 54.7k to 71.5k req/s; awaiting handler unchanged; mixed-app static file latency unchanged.
- A first prototype (GIL on the I/O thread, no gate) was measured and rejected: static file p50 2 ms to 22 ms in a mixed app.

- Allocation work (ADR-0023 addendum): `responder::impl` collapsed (30 to 22 mallocs per async request), `wire_response::keep` frees the state on the I/O thread.
  Awaiting `async def` +4 percent req/s, -8 percent CPU (under the 5 percent noise line for req/s). C++ 288 of 288, also under TSan.
  New C++ test: `requests_and_responders_dropped_on_other_threads_all_get_their_answer`.
- jemalloc bound: +15 percent on the awaiting handler, only about 10 percent of that is cross-thread frees. The mirror case (answer body freed on the I/O thread) is untouched.

## Open
1. Handlers that await still pay the hop. Options and why they are not done: ADR-0023 "Not done". Needs a measurement with a real handler first.
2. Not verified: Windows and macOS (the selector wrapper and the mutex gate are portable in principle). 3.14t is done: suite 618 passed, stress clean (ADR-0023).
3. Earlier list from hand-off 7, item 3 (FastAPI benchmark with dependencies, free-threading review, first batch of 8 blocking handlers, `listen()` signal wakeup fd, README items) is untouched.

## Tooling reminders
- This session had to rebuild everything: `uv venv venv13`, install build deps, `uv pip install --no-build-isolation --reinstall ./httpp`; C++ tests: `cmake --build build_cpp --target test_httpp_cpp` (the full default target fails in the nested Python sub-build; use the target).
- `oha` 1.16 from the GitHub release. The A/B script (`ab.py`) lived in the session scratchpad and is gone; it takes `ROUNDS PATH label=module:app ...`, reads `/proc` for CPU per request, and `AB_PY_<label>` picks another interpreter (for old-versus-new wheels).
