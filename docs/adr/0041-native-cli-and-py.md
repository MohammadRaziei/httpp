# ADR-0041: The command line is a native executable; Python commands live behind `py`

- **Status:** accepted
- **Source:** `src/cli/httpp_cli.cpp`, the `httpp_cli` target in `CMakeLists.txt` (`OUTPUT_NAME httpp`), `src/third_party/CLI11/CLI11.hpp`,
  `src/bindings/python/httpp/__main__.py`, `tests/python/test_cli.py`, `tests/python/conftest.py`

## Context

`httpp` was a Python console script. Every call, even `httpp browse` or `httpp download`, started an interpreter and ran with the GIL, although those commands only call C++.
The owner's idea (2026-11-06): move the command line to C++ and keep Python only where it is needed. Design discussion with the owner on 2026-11-07: `run` cannot be native (it imports
the user's module), and a second parser for `run` in C++ would have to be kept in step with the Python one.

## Decision

1. One native executable, `httpp` (CMake target `httpp_cli`, `OUTPUT_NAME httpp`), built with CLI11 v2.5.0 (one vendored header, unmodified; `CLI11_HAS_FILESYSTEM=0` for the macOS 10.13 target).
2. Native commands, no interpreter and no GIL: `httpp server`, `browse`, `download`, `curl`, and `--version`. Flags, defaults and messages are those of the old Python commands. New: `--shutdown-timeout` on
   `server` and `browse`, a port range check, usage errors exit with 2.
3. Python commands go behind `py`: `httpp py run main:app ...`, `httpp py path ...`. The options of the Python commands exist in one place only, `__main__.py`, and the `py` in the command is the reminder that the command needs Python.
   Who handles `py` depends on how `httpp` was started (below): the launcher runs it in its own process; the executable on its own (an SDK install) does not parse anything after `py` and starts `<python> -m httpp <rest>`
   (`exec` on POSIX, wait on Windows with the arguments quoted for the C runtime).
4. Which Python the executable starts for `py`: `$HTTPP_PYTHON`, else `python3` or `python` next to the executable, else `python3` or `python` on PATH. Nothing found: an error that says so and names `HTTPP_PYTHON`.
   The launcher sets `HTTPP_PYTHON` to its own interpreter, so inside the wheel the Python is always the one that has httpp.
5. `httpp run` and `httpp path` (the old top-level spelling) fail with exit 2 and a message that says `httpp py run`. There is no alias: a breaking change before 0.8.1 is released. `python -m httpp` is the Python half
   (`run`, `path`) and answers the native names with a message that says they are native.
6. Packaging (owner decision 2026-11-07, replacing a first design that put the executable in the scripts directory): the executable is **inside the package**, `httpp/bin/httpp` (`httpp.exe` on Windows), next to `httpp/lib/`, so
   the RPATH `$ORIGIN/../lib` (`@executable_path/../lib` on macOS) is the same for the wheel and for an SDK install (`<prefix>/bin`, `<prefix>/lib`) and there is one copy of `libhttpp_core`. `httpp` on PATH is the console
   script `httpp = httpp_cli:main` (`src/bindings/python/httpp_cli.py`), a module installed **next to the package in site-packages, not inside it**: a module inside `httpp/` would make Python import `httpp/__init__` (the compiled extension) before the launcher runs, 78 ms to start a program that does not need it. It finds the executable, gives it its execute bits if an installer dropped them (a wheel does not always keep file modes) and runs it:
   `os.execve` on POSIX (Python is replaced, nothing of it keeps running, so serving is not under the GIL), `subprocess.call` on Windows with `httpp/lib` put on PATH for the DLL. `python -m httpp browse` does the same (`python -m httpp` is the package's `__main__`, which imports the launcher).
   `httpp py ...` runs `httpp.__main__.main` inside the launcher (no second interpreter). `-DHTTPP_BUILD_CLI=OFF` skips the executable; the launcher then says it is missing.

## Rejected alternatives

- A C++ parser for `run` that re-executes Python with the parsed options (HANDOFF 6.1 as first written): a second definition of every option of `run`, a drift risk, one more process hop, for a gain of nothing (the work is Python).
- The executable in the scripts directory with a second copy of the core library beside it (the first design, built and tested, then dropped): the scripts directory is far from `httpp/lib/` and the way between them depends on the Python version and the platform, so the library had to be copied (1.5 MB compressed in the wheel) and `auditwheel` and `delocate` had to cope with a library in the scripts directory.
- A launcher that is only a shim, with the C++ `py` spawn as the way to Python: the launcher already is Python, so `py` runs in-process.
- Two independent implementations of `server`, `browse`, ... one in Python, one in C++: twice the bugs and tests.
- An alias `httpp run`: owner decision, `py` is a deliberate reminder and the command stays short enough.
- Linking the core statically into the executable: one more build of every source file (7 minutes here) or an object library with a Windows `dllexport` problem; the copy of the library costs only disk.
- A relative RPATH into `site-packages/httpp/lib`: the path is not fixed.

## Consequences

- A native command started through `httpp` costs one Python start before the `exec` (measured 25 ms against 2 ms for the executable itself, Linux, Python 3.13; 78 ms when the launcher was inside the package), then runs without Python and without the GIL. `httpp py ...` imports the package as it must.
  The launcher must not import `httpp` at module level (a test checks it). The cost of the layout: a top-level module `httpp_cli` in site-packages.
- The wheel holds one platform executable (383 KB, 170 KB compressed) and no second library; every wheel (`cp39-abi3`, `cp313t`, `cp314t`, `cp315t`) carries it.
- Run directly (not through the launcher), `httpp/bin/httpp py ...` has no Python next to it: it needs `HTTPP_PYTHON` or a PATH Python that has httpp.
- Breaking: `httpp run ...` is now `httpp py run ...`, `httpp path ...` is now `httpp py path ...` (CHANGELOG).
- `ponytail:` on Windows the Python is `python.exe` next to the executable or on PATH, no registry lookup (PEP 514).
- Verified (2026-11-07, sandbox, Linux x86_64): the wheel builds with `uv build`/`uv pip install`, `httpp/bin/httpp` and the console script are installed, a lost execute bit is restored by the launcher, an SDK install (`cmake --install`) gives a working
  `bin/httpp`; Python suite 779 passed, 4 skipped on 3.13 (every command test runs once through the console script and once on the executable).
- Not verified: Windows and macOS (the `py` spawn and quoting code, the launcher PATH entry for the DLL, `@executable_path`), `auditwheel repair` and `delocate` on an executable inside the package, Python 3.14 and 3.14t and 3.15t after this change,
  sanitizers on the executable, whether a wheel built by CI keeps the execute bit (the launcher repairs it either way).
