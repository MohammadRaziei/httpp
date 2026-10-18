# ADR-0001: httpp owns both layers (replacing FastAPI/Starlette and uvicorn)

- **Status:** accepted
- **Decided by:** the project owner
- **Source:** `docs/Architecture.md` sections 1 and 6, `handoff.md` section 2

## Context

In the usual Python stack (FastAPI on Starlette on uvicorn on a socket) there are several boundaries and each one costs something:
copies, thread hops and the GIL. If only one layer is made fast, the boundaries stay. Things such as memory shared between workers
or declarative work in C++ (static files, constant responses) are only possible when both the top and the bottom belong to us.

## Decision

httpp implements a **Starlette/FastAPI-like API** natively on its own engine and is also the **server** (replacing uvicorn).
It is started with `httpp run main:app` or `app.run()`. Compatibility is measured by running Starlette's own tests and, later, FastAPI's
on httpp (with an `httpp.testclient.TestClient` on a real loopback server).

## Rejected alternatives

- **Running stock FastAPI through an ASGI adapter.** It keeps the boundary and defeats the main goal ("remove the boundary").
  An ASGI/WSGI adapter is only for running third-party apps and is not part of the plan.

## Consequences

- Compatibility with FastAPI is our job and a lot of work (`HTTPException`, `APIRouter`, middleware, forms and uploads,
  `BackgroundTasks`, ... are not there yet; see `checklist.md`).
- The exact API scope (Starlette-like only, or FastAPI-like as well) is **still open** (`Architecture.md`, risk 4).
- "Replacement for FastAPI" is only true as far as a differential test against FastAPI shows; that test has not been written.

## Related

ADR-0002, ADR-0005, ADR-0006, ADR-0007
