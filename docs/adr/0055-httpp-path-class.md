# ADR-0055: `httpp::path`, a pathlib-shaped public path type

- **Status:** accepted
- **Decided by:** the project owner (the Python side should read like the C++ side: pathlib.Path there, an own path here)
- **Source:** `include/httpp/path.hpp`, `src/core/path.cpp`, `tests/cpp/test_path.cpp`, `tests/python/test_pathlib.py`

## Context

ADR-0054 gave the library an internal file system module on UTF-8 strings. Every public function that takes a path still took a `std::string`, while Python users hold a `pathlib.Path`. The two sides should look alike, and a C++ user outside the project needs a path type that does not drag in `<filesystem>` (macOS 10.13).

## Decision

- `httpp::path` is a public, installed header (`httpp/path.hpp`, exported). It is a UTF-8 string with '/' as separator, cleaned on construction like pathlib (repeated and trailing separators go). `std::string` and `const char*` convert to it implicitly, so every call that passed a string still compiles.
- pathlib-shaped members: `name`, `stem`, `suffix`, `parent`, `with_name`, `with_suffix`, `is_absolute`, `operator/`, `as_posix`, `as_uri`, `path::cwd()`, `absolute`, `exists`, `is_file`, `is_dir`, `size`, `mkdir`, `unlink`, `rename`, and `open(mode)` (a `std::fstream`, opened through the wide API on Windows), plus `read_text` / `write_text`. Not covered: iterating the parts, `relative_to`, `glob`, `resolve`.
- The disk members call `httpp::fs` (ADR-0054), which stays the internal low-level layer: no second implementation.
- Public API that names a file takes `const path&`: `download` (constructor and `output`), `sha256_file`, `set_log_file`, `set_ca_file` (client, async_client, `request::ca_file`), `server::set_tls`, `mount_file_browser` (root), and `response::file`, so a route serves a file with `res.file = dir / "page.html"`.
- Python does not get a path class of its own. Every path argument takes `str`, `bytes` or any `os.PathLike` (`os.fsencode` at the Cython boundary), and `file()` / `FileResponse` already did. A route cannot return a bare `Path` (a `str` is text; use `httpp.file(path)`).

## Consequences

- `response::file` changed type from `std::string` to `path`: assigning a string still works; code that read it as a string uses `.str()`.
- The Windows half (`path::native`, `open`) shares ADR-0054's caveat: written without a Windows machine.
- The parity test maps `path.hpp` to `None` (pathlib.Path is the Python counterpart).

## Related

ADR-0054
