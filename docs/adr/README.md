# Architecture Decision Records

Each file records one architectural decision with its **context, decision, rejected alternatives and consequences**. They were extracted from
`docs/Architecture.md`, `handoff.md` and the real code; wherever something is not run or verified yet, the ADR says so.
Things that CI runs and passes are written as facts; skipped tests are listed in ADR-0015.

| ADR | Decision | Status |
|---|---|---|
| [0001](0001-own-both-layers.md) | httpp owns both layers (replacing FastAPI/Starlette and uvicorn) | accepted |
| [0002](0002-standard-asyncio-loop.md) | `async def` handlers run on the standard asyncio loop | accepted |
| [0003](0003-engine-and-third-party.md) | Asio + llhttp + Mbed TLS; cpp-httplib only for the sync client | accepted |
| [0004](0004-zero-system-dependencies.md) | No system dependencies | accepted |
| [0005](0005-multiprocess-workers.md) | Fresh processes + `SO_REUSEPORT` (Linux) | accepted |
| [0006](0006-own-model-layer.md) | Pydantic-v2-shaped models with Cython validators | accepted |
| [0007](0007-openapi-from-one-plan.md) | OpenAPI from the same request plan | accepted |
| [0008](0008-swagger-ui-compiled-in.md) | Swagger UI compiled into the library | accepted |
| [0009](0009-hot-path.md) | Hot path: dispatch on the I/O thread, one timer, an indexed router | accepted |
| [0010](0010-mbedtls-threading.md) | Mbed TLS thread safety | accepted |
| [0011](0011-logging.md) | Request and bind-address logs in C++; defaults | accepted |
| [0012](0012-run-wins-over-listen.md) | `httpp run` wins over a `listen` inside the file | accepted |
| [0013](0013-packaging.md) | `cp39-abi3` + a separate wheel per free-threaded version | accepted |
| [0014](0014-coding-conventions.md) | STL-style `snake_case`, ponytail style, English only, no em dash | accepted |
| [0015](0015-testing-and-ci.md) | What is tested where; the list of skips | accepted |
| [0016](0016-hybrid-api-parity.md) | Hybrid C++/Python API parity and its open gaps | accepted as a goal, gaps open |

## Not decided yet

These have no ADR because they are not decisions yet. They are tracked in `docs/Architecture.md` section 7 and must be decided **with the project owner**:
the shared memory library and the scope of `app.shared`, the coupling level, the exact API scope (Starlette-like, or FastAPI-like as well),
removing the hop on the `async def` path, WebSocket backpressure, the sub-interpreter experiment, a lighter replacement for Swagger UI.

## Writing a new ADR

File name `NNNN-short-english-title.md`. Sections: status, context, decision, rejected alternatives, consequences (including what is **not** verified), related.
Do not edit an accepted ADR; when a decision changes, write a new ADR and mark the old one "superseded by ADR-NNNN".
