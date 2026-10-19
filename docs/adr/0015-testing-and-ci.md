# ADR-0015: Testing and CI: what runs where, and the list of skips

- **Status:** accepted (describes the current state; CI is green)
- **Date:** 2026-10-18
- **Source:** `.github/workflows/cmake.yml`, `wheels.yml`, `pyproject.toml`, `tests/`

## Decision

| Job | Linux | macOS | Windows |
|---|---|---|---|
| C++ build and tests (`cmake.yml`) | build + `ctest` | build + `ctest` | **build only** |
| Coverage (`test_httpp_cpp`, Codecov) | yes | no | no |
| Wheel + Python tests inside the wheel (`wheels.yml`) | yes | yes (universal2) | yes (AMD64 and x86) |

Wheels: `cp39` (abi3), `cp314t` and `cp315t` with `cibuildwheel` 4.3.1, and `cp313t` with 3.4.1. Python tests run with
`pytest {project}/tests` and only the `[test]` dependencies (`pytest`, `pytest-xdist`, `pytest-cov`).

## xfail

No test uses `xfail`. A test that fails is either fixed or (when it is a platform limit) skipped, as in the table below.
If an expected failure is ever added (for example slowloris before a connection cap exists), it must be recorded here.

## Skips (Python)

| Test | Skip condition | Where it runs |
|---|---|---|
| `test_run.py`: graceful stop with `on_shutdown`, request logging in the CLI, supervisor with SIGTERM | `win32` (SIGTERM is a hard kill there) | Linux, macOS |
| `test_run.py`: several workers on one port (2 tests, plus `workers=2` of the import-time test) | not Linux (`SO_REUSEPORT`) | Linux only |
| `test_run.py`: free-threading flag (the GIL is not turned on at import) | not a free-threaded Python, or `PYTHON_GIL=1` | the `cp313t`/`cp314t`/`cp315t` wheels |
| `test_client.py`: TLS against a real server | no `openssl` tool, or Windows | Linux, macOS |
| `test_embed_file.py` | no `cmake` or C++ compiler in PATH | wherever they exist (normally present in the wheel CI) |
| `test_validate_vs_pydantic.py` (242), `test_models_vs_pydantic.py` (36), one test in `test_openapi.py` | `pydantic` not installed | **nowhere in CI** |
| two tests in `test_params_depends.py`, one in `test_openapi.py` | `openapi_spec_validator` not installed | **nowhere in CI** |

## C++

Tests are not skipped; they have compile-time branches: `#ifndef _WIN32` for the raw POSIX socket tests in `test_server.cpp`
and `#ifdef _WIN32` for creating the lock directory (`_mkdir`/`_rmdir`) in `test_download.cpp`. Because Windows runs no C++ tests,
those branches are only compiled there.

## Consequences

- The differential tests against Pydantic and the `openapi.json` validation with `openapi_spec_validator` **do not run in CI**; that coverage is
  local only: `pip install pydantic openapi-spec-validator && pytest tests/python` (Linux, CPython 3.12, one core:
  **544 passed, 1 skipped**, the skipped one being the free-threading test; without these two packages 262 passed and 7 skipped). Why these two packages are not in
  the test extras is not recorded and has to be settled with the project owner (adding them to the wheel CI on free-threaded Pythons
  may require building their dependencies; this needs checking and is not a test result).
- Any new test that depends on SIGTERM, `SO_REUSEPORT` or `openssl` must carry the same skips.

## Related

ADR-0006, ADR-0010, ADR-0011, ADR-0012, ADR-0013
