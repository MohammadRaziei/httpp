# ADR-0054: An own file system module, `httpp::fs`, replaces std::filesystem

- **Status:** accepted; the module was merged into `httpp::path` by ADR-0058 (the design below holds, the names changed: `fs::stat(p)` is `path(p).stat()`, `make_dir` is `mkdir`, `list` is `iterdir`, `real` is `resolve`, `replace` is `rename`)
- **Decided by:** the project owner ("do not use filesystem; write our own")
- **Source:** `src/core/internal/fs.hpp`, `src/core/fs.cpp`, `tests/cpp/test_fs.cpp`, `tests/python/test_portability_sources.py`

## Context

The macOS wheels target 10.13, where `<filesystem>` is marked unavailable (it needs 10.15). A use of it compiles on Linux and Windows and fails only the macOS build, which is how the CI run of 2026-11-09 broke (`file_handler` in the
logging module). The code had also grown five private copies of the same small operations: `download.cpp` (mkdir, rmdir, rename, size, remove), `file_browser.cpp` (a POSIX branch and a `std::filesystem` Windows branch),
`server.cpp` (size, stat, `realpath`, a second `std::filesystem` Windows branch), the CLI (stat, `realpath`, `GetFullPathNameA`, access) and the logging module.

## Decision

One internal module, `httpp::fs`, on UTF-8 `std::string` paths, with the operations httpp needs and nothing more: `stat` / `lstat` (kind, size, mtime with nanoseconds, link), `real` (links resolved), `is_inside`, `is_root`, `list`,
`make_dir` (`created` / `existed` / `failed`, because directory creation is atomic and the download lock stands on it), `remove_dir`, `remove_file`, `replace` (atomic move over an existing file), `is_executable`, `join`,
`filename`, and `native()` for the standard streams. POSIX uses `stat`, `realpath`, `dirent`, `rename`; Windows uses the wide API (`GetFileAttributesExW`, `GetFinalPathNameByHandleW`, `FindFirstFileW`, `MoveFileExW`) on a
UTF-16 conversion, so the ANSI code page is never involved and a non-ASCII name works. Free functions, no `path` class: every caller already holds a string and nothing needed `operator/` or iteration.

All library and CLI code goes through it. `fs.cpp` is also compiled into the CLI (the core hides its internals) and into the C++ test program. `tests/python/test_portability_sources.py` fails when `std::filesystem` appears in `src/` or
`include/` outside `third_party`.

## Behaviour changes

- `is_inside` is a whole-path test (`/data2` is not inside `/data`) and treats the file system root correctly; the old POSIX helper answered false for everything below `/`.
- Download destination, `.part`, the lock directory and its `alive` file, the checksum temp file, TLS certificate files, `sha256_file`, static files and the CLI `-o` file open by their UTF-8 name on Windows (they used the ANSI code page).
- `file_browser.cpp` has one implementation instead of a POSIX one and a Windows one; only the Windows reserved-name check stays platform specific.

## Consequences

- The Windows half of `fs.cpp` was written without a Windows machine to run it on; CI on Windows is the first test of it. Same for macOS (`<filesystem>` is gone, so the 10.13 build should now pass).
- The C++ test programs still use `std::filesystem` for their own scratch files (they never ship).
- No recursive operations, permissions, copying or temporary names: add them here when something needs them.

## Related

ADR-0050 (the amendment), ADR-0051 to ADR-0053
