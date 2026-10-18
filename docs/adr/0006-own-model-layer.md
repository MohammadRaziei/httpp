# ADR-0006: Own model/validation layer shaped like Pydantic v2, with Cython validators

- **Status:** accepted
- **Source:** `src/bindings/python/httpp/models.py`, `_validate.py`, `_validate_cy.pyx`, `handoff.md`

## Context

FastAPI-style code (request bodies, response models, 422 errors) must work without installing Pydantic, and the wheel must stay small.

## Decision

- `httpp.models` is a Pydantic-v2-shaped subset: `BaseModel`, `Field`, `field_validator`/`model_validator`,
  `model_dump`, `model_validate`, `model_json_schema`, and errors with the same shape (`type`, `loc`, `msg`, `input`, `ctx`).
  A real Pydantic model is also accepted wherever a model is accepted.
- The validators are Cython `cdef class`es (`_validate_cy.pyx`). **Two passes over one code path:** a fast pass that builds no `loc`
  or message (the first error is returned); only if it fails is the same validator run again with `errors`, which builds
  Pydantic-shaped errors.

## Consequences

- A user `@field_validator` runs twice on **invalid** input, so it must not have side effects.
- Not supported: `validate_assignment`, computed fields, private attributes, custom serializers, strict mode, generics.
- Speed (same machine, same run, noisy machine): a valid `User` model went from 10,759 to 3,398 ns; `Union[int,str,None]` from
  954 to 159 ns; an invalid model with 5 errors from 17,280 to 11,305 ns.
- Equivalence with Pydantic is checked by 278 differential tests (242 in `test_validate_vs_pydantic.py` and 36 in
  `test_models_vs_pydantic.py`). They run only when Pydantic is installed and **do not run in CI** ([ADR-0015](0015-testing-and-ci.md)).
- `_validate_cy` is marked `freethreading_compatible=True`. A test in `test_run.py` checks on the `cp313t`, `cp314t` and `cp315t`
  wheels in CI that the GIL is not turned back on at import; it is skipped on ordinary Python.

## Related

ADR-0007, ADR-0013
