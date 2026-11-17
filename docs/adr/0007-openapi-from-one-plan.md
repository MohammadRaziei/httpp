# ADR-0007: OpenAPI is built from the same request plan

- **Status:** accepted
- **Source:** `src/bindings/python/httpp/_params.py`, `_schema.py`, `handoff.md`

## Context

If documentation and real behavior come from two sources, they drift apart sooner or later.

## Decision

When a route is registered, `_params.py` decides where each handler argument comes from, using FastAPI's rules: a name inside the path is a
path parameter (`{x}`, `<x>`, `:x`); a scalar or a list of scalars is a query parameter; anything else is the JSON body (one body).
`Query`, `Path`, `Header`, `Cookie`, `Body` and `Depends` enrich the same plan. **That same plan** builds both the request path (the 422 error as
`{"detail": [...]}`) and the `/openapi.json` document (OpenAPI 3.1). `_schema.py` runs once and in plain Python.

## Consequences

- Documentation and behavior cannot drift apart.
- The 422 for `int`/`float` path parameters still has the old `{"error": ...}` body, because an existing test depends on it
  (suggestion: change the test and move everything to `{"detail": [...]}`).
- If a route's own parameters are invalid, its dependencies are not run (only those errors are reported).
- Cleanup of `yield` dependencies happens after the handler and **before** the response is written (FastAPI's order differs slightly).
- Not there: forms and files, `Security()` (the FastAPI dependency that also extracts credentials), `servers`, response `examples`. Security schemes are declared for documentation only: `httpp.security.basic()` / `bearer()` / `api_key()`, `Server(security_schemes=..., security=...)`, a route's `security=`; the check itself stays with `BasicAuthMiddleware` or the caller's own code.

## Related

ADR-0006, ADR-0008
