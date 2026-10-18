# ADR-0004: No system dependencies; every dependency is vendored or a submodule

- **Status:** accepted
- **Decided by:** the project owner
- **Source:** `handoff.md` section 1, `Architecture.md` goal 4

## Decision

Nothing is installed with `apt`/`brew` or a system package. Every new dependency is either a vendored file or a git submodule. pip
dependencies are allowed only at **build time** (`scikit-build-core`, `cython>=3.1`, `cython-cmake`, `jsonschema`, `jinja2`; see `pyproject.toml`).
The wheel has no runtime dependencies.

## Consequences

- A from-scratch build on one core takes about 5 minutes for Python and about 5 minutes for the C++ tests (full Mbed TLS).
- configure generates the tf-psa-crypto sources with its own Python scripts and needs `jsonschema` and `jinja2`
  (without them configure ends with `Failed to generate tf-psa-crypto sources`; seen on a machine that lacked them).
- configure downloads `public_suffix_list.dat` once.

## Related

ADR-0003, ADR-0013
