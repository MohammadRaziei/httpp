# ADR-0041: The command line: a native executable, commands that only call the library, and HTTPS

- **Status:** accepted (merged ADR; each part keeps its own status line)
- **Merged on:** 2026-11-21, at the request of the project owner: the decisions of one subject were spread over 3 ADRs. The text of every part is kept, as a section below, in the order it was decided.

## Current state

Read this first. It says what holds today; the parts below give the reasons, the rejected alternatives and the history. Where a part and this summary disagree, this summary wins.

- **One native executable, `httpp`** (target `httpp_cli`, CLI11 vendored): `server`, `browse`, `download`, `curl` and `--version` run without Python and without the GIL. Commands that need Python live behind `py` (`httpp py run main:app`, `httpp py path`); the executable starts `$HTTPP_PYTHON`, else `python3` / `python` next to it, else on PATH.
- **The commands hold no logic of their own.** Binding, the URL string, the signal handling and drain loop (`server::bind`, `server::url()`, `server::listen_until_signal(grace)`), the log level by name, the banner (`httpp::banner()`, `print_banner`) and the hint for an encrypted key without its password are in the library; the CLI keeps only its options, defaults, exit codes, the environment variable for the key password and the decision to show the banner. `mount` takes and returns a `server` handle.
- **`httpp server` and `httpp browse` can serve HTTPS** with `--ssl-certfile`, `--ssl-keyfile` and `--ssl-keyfile-password` (also `HTTPP_SSL_KEYFILE_PASSWORD`); certificate and key must come together; the printed address says `https://`.
- The curl command has its own ADR (ADR-0077).

## Parts
### The command line is a native executable; Python commands live behind `py`

- **Status:** accepted
- **Source:** `src/cli/httpp_cli.cpp`, the `httpp_cli` target in `CMakeLists.txt` (`OUTPUT_NAME httpp`), `src/third_party/CLI11/CLI11.hpp`,
  `src/bindings/python/httpp/__main__.py`, `tests/python/test_cli.py`, `tests/python/conftest.py`

#### Context

`httpp` was a Python console script. Every call, even `httpp browse` or `httpp download`, started an interpreter and ran with the GIL, although those commands only call C++.
The owner's idea (2026-11-06): move the command line to C++ and keep Python only where it is needed. Design discussion with the owner on 2026-11-07: `run` cannot be native (it imports
the user's module), and a second parser for `run` in C++ would have to be kept in step with the Python one.

#### Decision

1. One native executable, `httpp` (CMake target `httpp_cli`, `OUTPUT_NAME httpp`), built with CLI11 v2.5.0 (one vendored header, unmodified; `CLI11_HAS_FILESYSTEM=0` for the macOS 10.13 target).
2. Native commands, no interpreter and no GIL: `httpp server`, `browse`, `download`, `curl`, and `--version`. Flags, defaults and messages are those of the old Python commands. New: `--shutdown-timeout` on
   `server` and `browse`, a port range check, usage errors exit with 2.
3. Python commands go behind `py`: `httpp py run main:app ...`, `httpp py path ...`. The options of the Python commands exist in one place only, `__main__.py`, and the `py` in the command is the reminder that the command needs Python.
   Who handles `py` depends on how `httpp` was started (below): the launcher runs it in its own process; the executable on its own (an SDK install) does not parse anything after `py` and starts `<python> -m httpp py <rest>`
   (`exec` on POSIX, wait on Windows with the arguments quoted for the C runtime).
4. Which Python the executable starts for `py`: `$HTTPP_PYTHON`, else `python3` or `python` next to the executable, else `python3` or `python` on PATH. Nothing found: an error that says so and names `HTTPP_PYTHON`.
   The launcher sets `HTTPP_PYTHON` to its own interpreter, so inside the wheel the Python is always the one that has httpp.
5. `httpp run` and `httpp path` (the old top-level spelling) fail with exit 2 and a message that says `httpp py run`. There is no alias: a breaking change before 0.8.1 is released. `python -m httpp ARGS` is the same program as `httpp ARGS` (owner decision 2026-11-07: one grammar, so `python -m httpp py run main:app`; the first version of `__main__` was only the Python half under the old names, which made the two differ).
6. Packaging (owner decision 2026-11-07, replacing a first design that put the executable in the scripts directory): the executable is **inside the package**, `httpp/sdk/bin/httpp` (`httpp.exe` on Windows), next to `httpp/lib/`, so
   the RPATH `$ORIGIN/../lib` (`@executable_path/../lib` on macOS) is the same for the wheel and for an SDK install (`<prefix>/bin`, `<prefix>/lib`) and there is one copy of `libhttpp_core`. `httpp` on PATH is the console
   script `httpp = httpp_cli:main` (`src/bindings/python/httpp_cli.py`), a module installed **next to the package in site-packages, not inside it**: a module inside `httpp/` would make Python import `httpp/__init__` (the compiled extension) before the launcher runs, 78 ms to start a program that does not need it. It finds the executable, gives it its execute bits if an installer dropped them (a wheel does not always keep file modes) and runs it:
   `os.execve` on POSIX (Python is replaced, nothing of it keeps running, so serving is not under the GIL), `subprocess.call` on Windows with `httpp/lib` put on PATH for the DLL. `python -m httpp ARGS` is the same program (the package's `__main__.main` calls the launcher's `main`).
   `httpp py ...` runs `httpp.__main__.py_main` inside the launcher (no second interpreter). `-DHTTPP_BUILD_CLI=OFF` skips the executable; the launcher then says it is missing.

#### Rejected alternatives

- A C++ parser for `run` that re-executes Python with the parsed options (HANDOFF 6.1 as first written): a second definition of every option of `run`, a drift risk, one more process hop, for a gain of nothing (the work is Python).
- The executable in the scripts directory with a second copy of the core library beside it (the first design, built and tested, then dropped): the scripts directory is far from `httpp/lib/` and the way between them depends on the Python version and the platform, so the library had to be copied (1.5 MB compressed in the wheel) and `auditwheel` and `delocate` had to cope with a library in the scripts directory.
- A launcher that is only a shim, with the C++ `py` spawn as the way to Python: the launcher already is Python, so `py` runs in-process.
- Two independent implementations of `server`, `browse`, ... one in Python, one in C++: twice the bugs and tests.
- An alias `httpp run`: owner decision, `py` is a deliberate reminder and the command stays short enough.
- Linking the core statically into the executable: one more build of every source file (7 minutes here) or an object library with a Windows `dllexport` problem; the copy of the library costs only disk.
- A relative RPATH into `site-packages/httpp/lib`: the path is not fixed.

#### Consequences

- A native command started through `httpp` costs one Python start before the `exec` (measured 25 ms against 2 ms for the executable itself, Linux, Python 3.13; 78 ms when the launcher was inside the package), then runs without Python and without the GIL. `httpp py ...` imports the package as it must.
  The launcher must not import `httpp` at module level (a test checks it). The cost of the layout: a top-level module `httpp_cli` in site-packages.
- The wheel holds one platform executable (383 KB, 170 KB compressed) and no second library; every wheel (`cp39-abi3`, `cp313t`, `cp314t`, `cp315t`) carries it.
- Run directly (not through the launcher), `httpp/sdk/bin/httpp py ...` has no Python next to it: it needs `HTTPP_PYTHON` or a PATH Python that has httpp.
- Breaking: `httpp run ...` is now `httpp py run ...`, `httpp path ...` is now `httpp py path ...` (CHANGELOG).
- `ponytail:` on Windows the Python is `python.exe` next to the executable or on PATH, no registry lookup (PEP 514).
- Verified (2026-11-07, sandbox, Linux x86_64): the wheel builds with `uv build`/`uv pip install`, `httpp/sdk/bin/httpp` and the console script are installed, a lost execute bit is restored by the launcher, an SDK install (`cmake --install`) gives a working
  `bin/httpp`; Python suite 779 passed, 4 skipped on 3.13 (every command test runs once through the console script and once on the executable).
  After `python -m httpp` became the same program as `httpp` (workers and the reload supervisor now spawn `-m httpp py run`; the executable on its own starts `<python> -m httpp py ...`): 782 passed, 4 skipped on 3.13 and 3.14, 781 passed, 5 skipped on 3.14t.
  Tests that need the pip-generated console script are skipped when the tree is not pip-installed (the CMake test target runs on a staged install directory); the same commands still run on the executable.
- Not verified: Windows and macOS (the `py` spawn and quoting code, the launcher PATH entry for the DLL, `@executable_path`), `auditwheel repair` and `delocate` on an executable inside the package, Python 3.15t after this change,
  sanitizers on the executable, whether a wheel built by CI keeps the execute bit (the launcher repairs it either way).

#### Addendum 2026-11-07: one `sdk` folder in the wheel (owner decision)
The wheel's C++ parts live in `httpp/sdk/`, the same shape as an SDK install prefix: `sdk/bin/httpp`, `sdk/include/`, `sdk/lib/libhttpp_core.*` and the CMake config in `sdk/lib/cmake/httpp/`. On Windows the DLL is in `sdk/bin` next to `httpp.exe` (the import library stays in `sdk/lib`): Windows has no RPATH, and a `httpp.exe` started directly (not through the launcher) died with 0xC0000135, DLL not found, when the DLL was in `sdk/lib` (CI of PR #164, cp39-win32). The package adds `sdk/bin` with `os.add_dll_directory`; the launcher no longer edits PATH.
`httpp/` itself holds only Python and the two extension modules. Consequences: the extension's RPATH is `$ORIGIN/sdk/lib` (`@loader_path/sdk/lib` on macOS), the executable's RPATH stays `$ORIGIN/../lib`, `httpp.get_include_dir()`, `get_lib_dir()` and `get_cmake_dir()` point into `sdk/`,
and `find_package(httpp CONFIG)` also works with `CMAKE_PREFIX_PATH=<site-packages>/httpp/sdk` (standard `lib/cmake/httpp` location). Breaking for anyone who hard-coded `httpp/lib` or `httpp/include` (CHANGELOG).
Verified on Linux: Python suite 783 passed on 3.13, 782 on 3.14t; `examples/find_httpp_via_python` builds and runs; a consumer using `CMAKE_PREFIX_PATH` builds and runs. Not verified: Windows (`os.add_dll_directory(sdk/lib)`), macOS (`@loader_path/sdk/lib`, `delocate`), `auditwheel repair` with the new RPATH.

### `httpp server` and `httpp browse` can serve HTTPS

- **Status:** accepted
- **Decided by:** the project owner (asked for TLS on the two commands that stay up)
- **Source:** `src/cli/httpp_cli.cpp` (`tls_args`, `add_tls_options`), `src/core/tls_session.cpp` (the encrypted-key hint), `tests/python/test_cli.py` (the TLS section), `docs/site/docs/server/serve-directory.mdx`, `docs/site/docs/server/file-browser.mdx`

#### Context

The core has served TLS since ADR-0040 (`server::set_tls(cert, key, password)`), and `httpp run` exposes it as `--ssl-certfile`, `--ssl-keyfile` and `--ssl-keyfile-password`. The two native commands that keep a server up, `server` and `browse`, had no way to use it, so sharing a folder over a network meant plain HTTP.

#### Decision

- Both commands get the same three options as `httpp run`, with the same names: `--ssl-certfile FILE`, `--ssl-keyfile FILE`, `--ssl-keyfile-password PASSWORD`. One shared helper adds them, so the two commands cannot drift apart.
- The certificate and the key come together: CLI11 refuses one without the other (exit 2), and refuses a file that does not exist (exit 2).
- The password also comes from `HTTPP_SSL_KEYFILE_PASSWORD`, so it can stay out of the process list (a command-line argument shows in `ps`). The option keeps the `httpp run` name and still works.
- `set_tls` runs while the server is set up, before it listens. Its errors (unreadable file, key not belonging to the certificate, wrong password) end the command with exit 1 and its message, which names the file. An encrypted key given without a password used to fail as "PEM expected"; `set_tls` now says the key is encrypted and no password was given (the part "the `httpp` commands call the library; `mount` returns the server" moved that hint from the CLI into the core).
- The address the command prints says `https://` when TLS is on (`server::url()`, the part "the `httpp` commands call the library; `mount` returns the server"). The core's own `listening on https://...` log line already did.

#### Rejected alternatives

- **Short names (`--cert`, `--key`).** Nicer to type, but a third spelling next to the two the project already has for the same thing. The names follow `httpp run`.
- **A `--key-password-file` option, no password argument.** Safer on its own, but it would be the only place the three commands differ. The environment variable gives the same protection with the existing name.
- **`--self-signed` to make a certificate on the fly.** Useful, but it is a feature of its own (key type, lifetime, names, where to keep it) and the core has no certificate generation. The documentation shows the one-line `openssl` command and `mkcert` instead.
- **Serving HTTP and HTTPS together, or redirecting.** One port serves one of them (ADR-0040); a second listener is a different command line and more state.

#### Consequences

- A plain `http://` request to a TLS port is dropped, as ADR-0040 says; the tests check it does not answer 200.
- The files are read once at start-up; a renewed certificate needs a restart (ADR-0040).
- The "encrypted key" hint is a search for `ENCRYPTED` in the key text, not a parse. The PKCS#8 `ENCRYPTED PRIVATE KEY` and the legacy `Proc-Type: 4,ENCRYPTED` forms both contain it; a key that does not would get the plain message.
- Not verified: Windows and macOS (the tests ran on Linux with the openssl command line making the certificate); a real browser's trust prompt for a self-signed certificate; certificates with a chain longer than the leaf; client certificates (the core has none).

#### Related

ADR-0040 (TLS in the core), the part "The command line is a native executable; Python commands live behind `py`" (the `httpp` command), ADR-0032 (the two commands' options).

### the `httpp` commands call the library; `mount` returns the server

- **Status:** accepted; the first decision (`mount` returns the `server&`) is superseded by ADR-0081 (`mount` takes and returns a `server` handle by value)
- **Decided by:** the project owner (the commands should only call functions, not hold logic)
- **Source:** `include/httpp/server.hpp` (`mount`, `bind`, `url`, `listen_until_signal`), `include/httpp/banner.hpp`, `include/httpp/logging.hpp` (`set_log_level(name)`), `src/core/tls_session.cpp`, `src/cli/httpp_cli.cpp` (`run_server`), `tests/cpp/test_server_lifecycle.cpp`

#### Context

the part "The command line is a native executable; Python commands live behind `py`" made `httpp` a command over the library. In practice `server` and `browse` had grown logic of their own that a C++ program would want too: the signal handlers and the drain loop, the table that turns `--log-level info` into a level, the `http://host:port/` string, the hint for an encrypted key without its password, and the banner. A program embedding the library had to copy them.

#### Decision

- **`mount` returns the server it was given.** `directory::mount`, `file_browser::mount` and `swagger_ui::mount` return `server&`, a reference to the argument (no copy; `server` is move-only), like `set_tls` already does. The chain goes on: `directory(p).mount(srv, "/").set_tls(cert, key)`. Existing calls that ignore the result compile unchanged.
- **`server::bind(host, port)`** binds without listening, so the address is known before the call that blocks. `listen(host, port)` is now `bind` then `listen_after_bind`. The "listening on ..." log line moved to the bind, so a port that is taken never says it.
- **`server::url()`** is `scheme://host:port/`: https after `set_tls`, the port that was really bound (so `bind(host, 0)` works), an IPv6 host in brackets (the CLI wrote `http://::1:8000/`, which is not a URL). It is `""` before a bind. The host is the one that was asked for, so `0.0.0.0` stays `0.0.0.0`.
- **`server::listen_until_signal(grace)`** is what the CLI's `serve()` was: after `bind()`, the first SIGINT/SIGTERM is `shutdown(grace)`, a second is `stop()`. It replaces the process's two handlers while it runs and puts the old ones back before it returns, so it belongs in one place per process.
- **`set_log_level(std::string_view)`** takes `trace`, `debug`, `info`, `warning`, `error` or `off` in any case, and returns false for another name. `level_from_string` could not serve: it belongs to the other level enum and does not know `off`.
- **`httpp::banner()` and `httpp::print_banner(FILE*)`** (`banner.hpp`) hold the mark, the version and the terminal test. Printing it is opt-in; the library never prints it by itself, because a program that embeds the library, or Python's `Server`, must not get ASCII art on stderr.
- **The hint for an encrypted key without its password** is in the core, where the key text is already in memory, not in the CLI: `set_tls` says the key is encrypted and no password was given.
- The CLI keeps what is about its own command line: the options, their defaults and checks, the exit codes, the environment variable for the key password, and the decision to show the banner (`--no-banner`, `--log-level off`). `run_server` is the one function the two commands share, and it is six calls.

#### Rejected alternatives

- **`directory::make_server()`.** A second way to the same result under a new name; `mount` already takes the server and the rest of the builders return `*this`.
- **`mount(base)` with no server argument, creating one.** It could not mount several things on one server, and it leaves open who owns the server.
- **Printing the banner from `listen`.** One surprise for every embedder.
- **Binding the new names in Python.** `Server.run` already binds, serves and stops on a signal, and Python knows the host and port it passed. The three names are in `NOT_EXPOSED` with that reason (ADR-0016), and `banner.hpp` maps to `None`.

#### Consequences

- A C++ program gets the same lifecycle as the commands in a few calls.
- The log line now says `(bind)` instead of `(listen)` as its source.
- `mount`'s return type changed from `void`: source compatible, but not binary compatible, and the library is still before 1.0.
- `url()` and the IPv6 brackets are tested for a machine with IPv6 loopback only (the test returns early without one; this machine had none, so the bracket case was never run).
- `listen_until_signal` installs process-wide handlers. Two servers listening this way at once would share one signal counter.

#### Related

the part "The command line is a native executable; Python commands live behind `py`" (the `httpp` command), ADR-0040 (TLS), the part "`httpp server` and `httpp browse` can serve HTTPS" (HTTPS for the two commands), ADR-0016 (`NOT_EXPOSED`).
