# ADR-0035: The answer hook path does not touch the server object

- **Status:** accepted
- **Source:** `src/core/server.cpp` (`config::pool`, `make_sender`, `answer_with_hooks`), `tests/cpp/test_server_io_routes.cpp`

## Context

An answer can arrive after the `server` is gone (a `responder` kept by user code, ADR-0033 item 2). With an `after` hook, an error handler or compression, `make_sender`
returned a lambda that locked a `weak_ptr<config>` and then, for a `route_io` handler that answered on the spot, used `this` (the server implementation) to reach the
worker pool and to call `make_sender` again from the queued job. Locking the config does not keep the server alive: the server owns the config, not the other way round.
If the server was destroyed between `weak.lock()` and the use of `this`, the lambda read freed memory. Found by reading the code (handoff section 12, item 4); no crash
was ever observed, the window is narrow.

A second, smaller defect was in the same lines: the job took `res` by move, and when the pool refused it (shutting down) the code went on with the moved-from `res`.

## Decision

- The pointer to the running pool lives in `config` (`config::pool`, an atomic); the server implementation keeps a reference to it. Everything the hook path needs is now in
  the shared config.
- The hook path is a static function, `answer_with_hooks(config, request, answer, response, ...)`. The queued job holds a `shared_ptr<config>` and calls the same function with
  "not on the I/O thread", so it needs no `this` and runs the hooks once, on a worker.
- When the pool refuses the job, the job itself runs on the current thread (it still owns the response), instead of using a moved-from response.

No API change, no change when no hook is set (that path never held the config, ADR-0033 item 9).

## Rejected alternatives

- Keep `this` and make the server wait for pending answers in its destructor: a user-held `responder` can answer at any time, so the destructor would wait for user code.
- A `shared_ptr` to the whole server implementation in every sender: one more shared count per answer on every path, which ADR-0033 removed on purpose.

## Consequences

- Left as it is: `worker_pool*` itself. A thread can load the pointer just before `listen_after_bind` sets it to null and destroys the pool; `enqueue` on a destroyed pool is
  still possible in that window. The engine threads are joined before that point, so only answers from foreign threads are exposed. A real fix needs the pool to be shared
  (`shared_ptr`) or a reader count; not done here, to be decided separately.
- Verified: the whole C++ suite passes (392 tests including the new stress test) and the `httpp_server_io*` tests are clean under AddressSanitizer.
- Honest limit: the new test is a smoke test, not a reproduction. Run three times under ASan against the code from before this change it also passed, so the window is too narrow
  for this test to hit. The fix rests on reading the code, not on a failing test.
