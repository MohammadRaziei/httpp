# ADR-0107: `httpp::env`, environment variables as UTF-8 text with a scope that restores them

- **Status:** implemented in C++ (built and tested on Linux); the Windows branch (`_wgetenv_s`, `_wputenv_s`) is written without a Windows machine to run it on; the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-22
- **Decided by:** the project owner asked whether `httpp::env` should exist, that it belongs in `utils`, and whether the names should be `httpp::getenv`, `setenv`, `environ`; then asked the implementer to take the best decision.
- **Source:** `include/httpp/env.hpp`, `src/utils/env.cpp`, `src/utils/internal/windows_text.hpp`, `src/core/proxy.cpp`, `src/utils/path.cpp` (`temp_dir`), `src/utils/logging.cpp` (`NO_COLOR`), `src/cli/httpp_cli.cpp` (`HTTPP_PYTHON`), `tests/cpp/test_env.cpp`, `tests/cpp/test_proxy.cpp`, `tests/cpp/test_path.cpp`
- **Follows:** ADR-0054, ADR-0086, ADR-0067

## Context

Four places read the environment with `std::getenv`: the proxy variables (`proxy.cpp`), `TMPDIR` / `TMP` / `TEMP` (`path::temp_dir`, ADR-0054), `NO_COLOR` (logging) and `HTTPP_PYTHON` (the CLI). Two of them wrote the same loop, "the first variable that is set and not empty" (`environment_value` in `proxy.cpp`, and the loop in `temp_dir`). `tests/cpp/test_proxy.cpp` had its own `set_env` with an `#ifdef` for `_putenv_s` / `setenv`, and its "clean environment" put the variables back **unset**, not as it found them, so a machine with `HTTP_PROXY` set lost it for the rest of the run.

On Windows `std::getenv` reads the narrow copy in the code page. A value with a character the code page lacks (a proxy URL with a user name in another script) comes back damaged, while the rest of `httpp_utils` is UTF-8 on every platform and goes through the wide API (ADR-0054).

POSIX says nothing about `getenv` running while another thread calls `setenv`: the C library may reallocate `environ`, and a reader can scan freed memory. A lock inside httpp cannot stop a `getenv` made by other code.

## Decision

1. **A namespace `httpp::env` in `httpp_utils`** (`include/httpp/env.hpp`, `src/utils/env.cpp`, `HTTPP_UTILS_API`): `get(name)` returns `std::optional<std::string>`; `first({names...})` returns the first value that is set and not empty, else `""`; `set(name, value)` and `unset(name)` return whether it was done; `scope` is the RAII class below.
2. **Names and values are UTF-8.** On Windows each call goes through `_wgetenv_s` / `_wputenv_s` (the CRT's wide environment, the copy `getenv` also reads), converted with `widen` / `narrow`, which moved from `path.cpp` to `internal/windows_text.hpp` so both files use one copy.
3. **One lock, in the library.** `get` copies the value under it, so a value read here is never torn by a `set` made here. The lock lives in `env.cpp` and not in the header on purpose: an inline function with a static in a header gets one copy per DLL on Windows, so it would not be one lock.
4. **`scope(name, value)` and `scope(name)`** set or remove a variable for the life of the object and put back what was there (the old value, or no value). Movable, so a container can hold several (`std::vector<env::scope>`); `restore()` does it early and only once.
5. **The four readers use it:** `proxy.cpp` and `temp_dir` call `env::first` / `env::get` (the duplicate loop is gone), logging reads `NO_COLOR` with `env::first`, the CLI reads `HTTPP_PYTHON`. `test_proxy.cpp` keeps its `set_env` as a three-line wrapper, and its clean environment is a vector of scopes, so the values the machine had come back.
6. **One header per topic, no `utils.hpp`.** The headers of `httpp_utils` are `logging.hpp`, `path.hpp`, `progress.hpp`; "utils" names the library (ADR-0086), not a header. `env.hpp` follows that.

## Alternatives considered

- **`httpp::getenv`, `httpp::setenv`, `httpp::unsetenv`, `httpp::environ`:** `getenv`, `setenv` and `unsetenv` are libc names, so under `using namespace httpp` a call is ambiguous or silently picks the other one, and some Windows compatibility headers define `setenv` as a macro. `environ` is a global variable on POSIX and a macro (`_environ`) on MSVC, so the name cannot be used at all. One namespace also keeps `get`, `set`, `first` and `scope` together, as `path::` and `temp_path` do, and reads like `os.environ` / `os.getenv`.
- **Header-only:** the Windows branch needs `windows.h`, and the lock has to be one per process (decision 3).
- **A class `env` with static members, like `path::cwd()`:** nothing here has state to hold; a namespace says that.
- **Making the lock cover everyone:** impossible. Only `getenv` calls made through httpp take it.

## Consequences

- Nothing that worked changes meaning; the proxy tests, the logging test and the CLI behave as before. A machine that had proxy variables set keeps them after the proxy tests (they were lost before).
- The lock does not make the C library safe: set variables before starting threads (or in a test), and read them through `get`. `env.hpp` says so.
- On Windows an empty value cannot be kept (`set(name, "")` removes the variable, so `get` says `nullopt` where POSIX says `""`). `first` treats empty as not set everywhere, so nothing in httpp depends on the difference.
- Not run here: Windows and macOS, and the Python suite.

## Taken by default (for the owner to confirm)

- `get` keeps "set and empty" apart from "not set" on POSIX; `first` skips both.
- `scope(name)` with no value means "remove it while the scope lives".
- `set` and `unset` return `bool` and never throw; `get` of a name that cannot be a variable (empty, or with `=`) is `nullopt`.
