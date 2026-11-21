# ADR-0054: Paths and the file system: no `std::filesystem`, one public `httpp::path`

- **Status:** accepted (merged ADR; each part keeps its own status line)
- **Merged on:** 2026-11-21, at the request of the project owner: the decisions of one subject were spread over 3 ADRs. The text of every part is kept, as a section below, in the order it was decided.

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history. Where a part and this summary disagree, this summary wins.

- **No `std::filesystem` anywhere** (the macOS wheels target 10.13, where it is unavailable); a test fails the build when it appears in `src/` or `include/`.
- **One public type, `httpp::path`** (`httpp/path.hpp`): a pathlib-shaped UTF-8 path with `/` as separator that also holds the disk operations (`stat`, `lstat`, `resolve`, `is_relative_to`, `iterdir`, `mkdir`, `rmdir`, `unlink`, `rename`, `open`, `read_text`, `write_text`, ...). The internal `httpp::fs` module no longer exists; its names map as `real` to `resolve`, `list` to `iterdir`, `make_dir` to `mkdir`, `replace` to `rename`, `join` to `operator/`, `filename` to `name`.
- **Every public function that names a file takes `const path&`;** a `std::string` converts implicitly. Python takes `str`, `bytes` or any `os.PathLike` and has no path class of its own.
- **Concurrent access:** `write_text_atomic` (write beside the target, then rename) for files that others may read; `file_lock` (`httpp/file_lock.hpp`, Python `httpp.FileLock`) is a separate class for mutual exclusion.
- **`mkdir()` is true only when this call created the directory.**
- The Windows half of this code was written without a Windows machine.

## Parts
### An own file system module, `httpp::fs`, replaces std::filesystem

- **Status:** accepted; the module was merged into `httpp::path` by the part "The file system module is merged into `httpp::path`" (the design below holds, the names changed: `fs::stat(p)` is `path(p).stat()`, `make_dir` is `mkdir`, `list` is `iterdir`, `real` is `resolve`, `replace` is `rename`)
- **Decided by:** the project owner ("do not use filesystem; write our own")
- **Source:** `src/core/internal/fs.hpp`, `src/core/fs.cpp`, `tests/cpp/test_fs.cpp`, `tests/python/test_portability_sources.py`

#### Context

The macOS wheels target 10.13, where `<filesystem>` is marked unavailable (it needs 10.15). A use of it compiles on Linux and Windows and fails only the macOS build, which is how the CI run of 2026-11-09 broke (`file_handler` in the
logging module). The code had also grown five private copies of the same small operations: `download.cpp` (mkdir, rmdir, rename, size, remove), `file_browser.cpp` (a POSIX branch and a `std::filesystem` Windows branch),
`server.cpp` (size, stat, `realpath`, a second `std::filesystem` Windows branch), the CLI (stat, `realpath`, `GetFullPathNameA`, access) and the logging module.

#### Decision

One internal module, `httpp::fs`, on UTF-8 `std::string` paths, with the operations httpp needs and nothing more: `stat` / `lstat` (kind, size, mtime with nanoseconds, link), `real` (links resolved), `is_inside`, `is_root`, `list`,
`make_dir` (`created` / `existed` / `failed`, because directory creation is atomic and the download lock stands on it), `remove_dir`, `remove_file`, `replace` (atomic move over an existing file), `is_executable`, `join`,
`filename`, and `native()` for the standard streams. POSIX uses `stat`, `realpath`, `dirent`, `rename`; Windows uses the wide API (`GetFileAttributesExW`, `GetFinalPathNameByHandleW`, `FindFirstFileW`, `MoveFileExW`) on a
UTF-16 conversion, so the ANSI code page is never involved and a non-ASCII name works. Free functions, no `path` class: every caller already holds a string and nothing needed `operator/` or iteration.

All library and CLI code goes through it. `fs.cpp` is also compiled into the CLI (the core hides its internals) and into the C++ test program. `tests/python/test_portability_sources.py` fails when `std::filesystem` appears in `src/` or
`include/` outside `third_party`.

#### Behaviour changes

- `is_inside` is a whole-path test (`/data2` is not inside `/data`) and treats the file system root correctly; the old POSIX helper answered false for everything below `/`.
- Download destination, `.part`, the lock directory and its `alive` file, the checksum temp file, TLS certificate files, `sha256_file`, static files and the CLI `-o` file open by their UTF-8 name on Windows (they used the ANSI code page).
- `file_browser.cpp` has one implementation instead of a POSIX one and a Windows one; only the Windows reserved-name check stays platform specific.

#### Consequences

- The Windows half of `fs.cpp` was written without a Windows machine to run it on; CI on Windows is the first test of it. Same for macOS (`<filesystem>` is gone, so the 10.13 build should now pass).
- The C++ test programs still use `std::filesystem` for their own scratch files (they never ship).
- No recursive operations, permissions, copying or temporary names: add them here when something needs them.

#### Related

ADR-0011 (the amendment), ADR-0051 to ADR-0051

### `httpp::path`, a pathlib-shaped public path type

- **Status:** accepted
- **Decided by:** the project owner (the Python side should read like the C++ side: pathlib.Path there, an own path here)
- **Source:** `include/httpp/path.hpp`, `src/core/path.cpp`, `tests/cpp/test_path.cpp`, `tests/python/test_pathlib.py`

#### Context

the part "An own file system module, `httpp::fs`, replaces std::filesystem" gave the library an internal file system module on UTF-8 strings. Every public function that takes a path still took a `std::string`, while Python users hold a `pathlib.Path`. The two sides should look alike, and a C++ user outside the project needs a path type that does not drag in `<filesystem>` (macOS 10.13).

#### Decision

- `httpp::path` is a public, installed header (`httpp/path.hpp`, exported). It is a UTF-8 string with '/' as separator, cleaned on construction like pathlib (repeated and trailing separators go). `std::string` and `const char*` convert to it implicitly, so every call that passed a string still compiles.
- pathlib-shaped members: `name`, `stem`, `suffix`, `parent`, `with_name`, `with_suffix`, `is_absolute`, `operator/`, `as_posix`, `as_uri`, `path::cwd()`, `absolute`, `exists`, `is_file`, `is_dir`, `size`, `mkdir`, `unlink`, `rename`, and `open(mode)` (a `std::fstream`, opened through the wide API on Windows), plus `read_text` / `write_text`. Not covered: iterating the parts, `relative_to`, `glob`, `resolve`.
- The disk members call `httpp::fs` (the part "An own file system module, `httpp::fs`, replaces std::filesystem"), which stays the internal low-level layer: no second implementation. (Superseded by the part "The file system module is merged into `httpp::path`": `fs` was merged into `path`.)
- Public API that names a file takes `const path&`: `download` (constructor and `output`), `sha256_file`, `set_log_file`, `set_ca_file` (client, async_client, `request::ca_file`), `server::set_tls`, `mount_file_browser` (root), and `response::file`, so a route serves a file with `res.file = dir / "page.html"`.
- Concurrent access (added with the atomic write): `path` has no lock inside, since a process-wide mutex does not protect against other processes, serializes all file I/O and does not survive a crash. `write_text()` / `read_text()` stay as they are (like pathlib) and are not thread safe. `write_text_atomic(data)` is the protocol for a file others may read meanwhile: write `<name>.tmp.<random>.<n>` beside the target, then `rename()` over it (atomic on POSIX and Windows), so readers see the whole old text or the whole new one and a failure leaves the old file untouched and no temporary file behind; two writers at once do not corrupt it (the last rename wins). It is not read-modify-write (take a lock for that: download's `<dest>.lock` directory, which stays private to `download.cpp`) and it does not `fsync`. `read_text(start, end)` (a byte range of the file, cut at its end; the whole file without arguments) moved here from `download.cpp`, which uses it (download reads its checksum line and a lock's heartbeat with `read_text(0, n)` and treats a missing file as empty).
- Mutual exclusion is a separate class, `httpp::file_lock` (`httpp/file_lock.hpp`, Python `httpp.FileLock`), not a member of `path`: Python's path is `pathlib`, which cannot gain one. `file_lock(target, stale_after = 15 s)` takes the atomic `mkdir` of `<target>.lock` and waits while another holder lives. The holder rewrites `alive` (`pid n`) every second; a waiter judges liveness by that content changing on its own clock (not by mtimes), and after `stale_after` without change removes the dead lock and takes it. If the lock directory cannot be made at all (read-only parent), the code goes on unlocked (`owned()` is false) instead of blocking. A lock held for a long time needs the heartbeat because a dead owner leaves the directory forever; `write_text_atomic` holds nothing, so it needs none. OS locks (`flock`, `LockFileEx`) were not used: they are unreliable on NFS. `download` uses it for its destination.
- Python does not get a path class of its own. Every path argument takes `str`, `bytes` or any `os.PathLike` (`os.fsencode` at the Cython boundary), and `file()` / `FileResponse` already did. A route cannot return a bare `Path` (a `str` is text; use `httpp.file(path)`).

#### Consequences

- `response::file` changed type from `std::string` to `path`: assigning a string still works; code that read it as a string uses `.str()`.
- The Windows half (`path::native`, `open`) shares the part "An own file system module, `httpp::fs`, replaces std::filesystem"'s caveat: written without a Windows machine.
- The parity test maps `path.hpp` to `None` (pathlib.Path is the Python counterpart), so the two new members need no Python side.
- `file_lock` is not verified on Windows or macOS; two waiters that both judge the same lock stale in the same instant can both take it (the header says so).
- `write_text_atomic` is not verified on Windows (the rename-replace is the one download already uses) and does not `fsync`.

#### Related

the part "An own file system module, `httpp::fs`, replaces std::filesystem"

### The file system module is merged into `httpp::path`

- **Status:** accepted; amends the part "An own file system module, `httpp::fs`, replaces std::filesystem" and the part "`httpp::path`, a pathlib-shaped public path type"
- **Decided by:** the project owner ("why is fs.hpp still there? merge it with path")
- **Source:** `include/httpp/path.hpp`, `src/core/path.cpp`, `tests/cpp/test_path.cpp`

#### Context

the part "An own file system module, `httpp::fs`, replaces std::filesystem" added an internal `httpp::fs` (free functions on `std::string`), the part "`httpp::path`, a pathlib-shaped public path type" a public `httpp::path` on top of it. That left two surfaces for one job: `path` forwarded to `fs`, and the library code still spoke `fs` with strings. The internal one was a layer nobody outside needed, and the CLI and the test program each compiled `fs.cpp` again because it was hidden.

#### Decision

- `src/core/internal/fs.hpp`, `src/core/fs.cpp` and `tests/cpp/test_fs.cpp` are gone; their code lives in `path.cpp` (POSIX and Windows halves unchanged) and their tests in `test_path.cpp`.
- The operations became pathlib-named members of `path`: `stat()` / `lstat()` (with `path::status` and `path::kind`), `resolve()` (was `real`), `is_relative_to(root)` (was `is_inside`), `is_root()`, `iterdir(entries)` (was `list`), `mkdir()`, `rmdir()`, `unlink()`, `rename(to)` (was `replace`), `is_executable()`, `name()` (was `filename`), `operator/` (was `join`), `native()`. `cwd()` and `absolute()` were already there.
- The library, the server, the file browser, downloads, TLS loading, `sha256_file` and the CLI use `httpp::path`. The CLI and the tests link `httpp_core`: no file is compiled twice.
- One behaviour change: `mkdir()` is true only when this call created the directory (it was "is there afterwards"). The download lock tells "there already" from "cannot be made" with `is_dir()`, which also means a plain file where the lock directory should be now runs without a lock instead of waiting on it.

#### Consequences

- The file system surface is public (it was internal): `status`, `kind` and the operations are API now.
- Windows half still written without Windows; same caveat as the part "An own file system module, `httpp::fs`, replaces std::filesystem".

#### Related

the part "An own file system module, `httpp::fs`, replaces std::filesystem" and the part "`httpp::path`, a pathlib-shaped public path type"
