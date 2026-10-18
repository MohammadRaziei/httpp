# ADR-0013: Packaging: `cp39-abi3` plus a separate wheel for each free-threaded version

- **Status:** accepted
- **Source:** `Architecture.md` principle 5, `.github/workflows/wheels.yml`, `handoff.md`

## Decision

- One `cp39-abi3` wheel for ordinary Pythons (Cython with `Py_LIMITED_API=0x030900f0`).
- Free-threaded Python (3.13t, 3.14t, ...) has no stable ABI; each version gets its own wheel (`cp313t`, `cp314t`, `cp315t`).
  `abi3t` arrives with CPython 3.15.
- "One wheel for everywhere" is wrong: one wheel per operating system and architecture is needed.
- `PIP_PREFER_BINARY=1` in CI so that `rpds-py` (a dependency of `jsonschema`) is not built from source with Rust.
- CI: `cibuildwheel` 4.3.1 builds `cp39` (abi3), `cp314t` and `cp315t` on Linux, macOS (universal2) and Windows (AMD64 and x86);
  `cp313t` comes from a separate step with `cibuildwheel` 3.4.1, because the 4.x series dropped free-threaded 3.13. Every wheel is tested with
  `pytest {project}/tests` (extras: `pytest`, `pytest-xdist`, `pytest-cov`) and all of them are green.

## Consequences

- Size: `Architecture.md` used to say about 1.1 MB, but the published 0.7.0 wheel on PyPI (Linux, cp312) is **1.8 MB** by `pip-size`
  and has no runtime dependencies; the old number was stale and has been corrected.
- CI does not run the C++ tests on Windows (build only); the Python tests inside the wheels do run on Windows.

## Related

ADR-0004, ADR-0006
