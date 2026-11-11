# ADR-0058: The file system module is merged into `httpp::path`

- **Status:** accepted; amends ADR-0054 and ADR-0055
- **Decided by:** the project owner ("why is fs.hpp still there? merge it with path")
- **Source:** `include/httpp/path.hpp`, `src/core/path.cpp`, `tests/cpp/test_path.cpp`

## Context

ADR-0054 added an internal `httpp::fs` (free functions on `std::string`), ADR-0055 a public `httpp::path` on top of it. That left two surfaces for one job: `path` forwarded to `fs`, and the library code still spoke `fs` with strings. The internal one was a layer nobody outside needed, and the CLI and the test program each compiled `fs.cpp` again because it was hidden.

## Decision

- `src/core/internal/fs.hpp`, `src/core/fs.cpp` and `tests/cpp/test_fs.cpp` are gone; their code lives in `path.cpp` (POSIX and Windows halves unchanged) and their tests in `test_path.cpp`.
- The operations became pathlib-named members of `path`: `stat()` / `lstat()` (with `path::status` and `path::kind`), `resolve()` (was `real`), `is_relative_to(root)` (was `is_inside`), `is_root()`, `iterdir(entries)` (was `list`), `mkdir()`, `rmdir()`, `unlink()`, `rename(to)` (was `replace`), `is_executable()`, `name()` (was `filename`), `operator/` (was `join`), `native()`. `cwd()` and `absolute()` were already there.
- The library, the server, the file browser, downloads, TLS loading, `sha256_file` and the CLI use `httpp::path`. The CLI and the tests link `httpp_core`: no file is compiled twice.
- One behaviour change: `mkdir()` is true only when this call created the directory (it was "is there afterwards"). The download lock tells "there already" from "cannot be made" with `is_dir()`, which also means a plain file where the lock directory should be now runs without a lock instead of waiting on it.

## Consequences

- The file system surface is public (it was internal): `status`, `kind` and the operations are API now.
- Windows half still written without Windows; same caveat as ADR-0054.

## Related

ADR-0054, ADR-0055
