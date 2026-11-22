# ADR-0107: `httpp::env` and `utils.hpp`: environment variables as UTF-8 text with a scope that restores them, SHA-256 without a TLS library

- **Status:** implemented in C++ (built and tested on Linux); the Windows branch (`_wgetenv_s`, `_wputenv_s`) is written without a Windows machine to run it on, and the Python suite (including the C++/Python parity test) was not run here; the points under "Taken by default" are for the owner to confirm
- **Date:** 2026-11-22
- **Decided by:** the project owner asked whether `httpp::env` should exist, that it belongs in `utils`, and whether the names should be `httpp::getenv`, `setenv`, `environ`; then asked the implementer to take the best decision. After the first version (`env.hpp`, `env.cpp`) the owner asked that `sha256` and `env` live together in `utils.hpp` and `utils.cpp` instead of in files of their own, and approved a SHA-256 that does not use Mbed TLS so that they can.
- **Source:** `include/httpp/utils.hpp`, `src/utils/utils.cpp`, `include/httpp/sha256.hpp` (now only includes `utils.hpp`), `src/utils/internal/windows_text.hpp`, `src/core/proxy.cpp`, `src/utils/path.cpp` (`temp_dir`), `src/utils/logging.cpp` (`NO_COLOR`), `src/cli/httpp_cli.cpp` (`HTTPP_PYTHON`), `tests/cpp/test_utils.cpp`, `tests/cpp/test_proxy.cpp`, `tests/cpp/test_path.cpp`, `tests/python/test_cpp_python_parity.py`
- **Follows:** ADR-0054, ADR-0086, ADR-0067

## Context

Four places read the environment with `std::getenv`: the proxy variables (`proxy.cpp`), `TMPDIR` / `TMP` / `TEMP` (`path::temp_dir`, ADR-0054), `NO_COLOR` (logging) and `HTTPP_PYTHON` (the CLI). Two of them wrote the same loop, "the first variable that is set and not empty" (`environment_value` in `proxy.cpp`, and the loop in `temp_dir`). `tests/cpp/test_proxy.cpp` had its own `set_env` with an `#ifdef` for `_putenv_s` / `setenv`, and its "clean environment" put the variables back **unset**, not as it found them, so a machine with `HTTP_PROXY` set lost it for the rest of the run.

On Windows `std::getenv` reads the narrow copy in the code page. A value with a character the code page lacks (a proxy URL with a user name in another script) comes back damaged, while the rest of `httpp_utils` is UTF-8 on every platform and goes through the wide API (ADR-0054).

POSIX says nothing about `getenv` running while another thread calls `setenv`: the C library may reallocate `environ`, and a reader can scan freed memory. A lock inside httpp cannot stop a `getenv` made by other code.

## Decision

1. **A namespace `httpp::env` in `httpp_utils`** (`include/httpp/utils.hpp`, `src/utils/utils.cpp`, `HTTPP_UTILS_API`): `get(name)` returns `std::optional<std::string>`; `first({names...})` returns the first value that is set and not empty, else `""`; `set(name, value)` and `unset(name)` return whether it was done; `scope` is the RAII class below.
2. **Names and values are UTF-8.** On Windows each call goes through `_wgetenv_s` / `_wputenv_s` (the CRT's wide environment, the copy `getenv` also reads), converted with `widen` / `narrow`, which moved from `path.cpp` to `internal/windows_text.hpp` so both files use one copy.
3. **One lock, in the library.** `get` copies the value under it, so a value read here is never torn by a `set` made here. The lock lives in `env.cpp` and not in the header on purpose: an inline function with a static in a header gets one copy per DLL on Windows, so it would not be one lock.
4. **`scope(name, value)` and `scope(name)`** set or remove a variable for the life of the object and put back what was there (the old value, or no value). Movable, so a container can hold several (`std::vector<env::scope>`); `restore()` does it early and only once.
5. **The four readers use it:** `proxy.cpp` and `temp_dir` call `env::first` / `env::get` (the duplicate loop is gone), logging reads `NO_COLOR` with `env::first`, the CLI reads `HTTPP_PYTHON`. `test_proxy.cpp` keeps its `set_env` as a three-line wrapper, and its clean environment is a vector of scopes, so the values the machine had come back.
6. **`env` and SHA-256 share `utils.hpp` and `utils.cpp`.** Both are small and need nothing but the standard library and the operating system, so they get one header and one source instead of one each. `logging`, `path` and `progress` are bigger and keep their own. The rule for `utils.hpp`: small helpers with no dependencies; anything with its own size or state gets a header. (The first version of this ADR had `env.hpp` and `env.cpp`; `env.hpp` was never released and is gone.)
7. **SHA-256 moves from `httpp_core` into `httpp_utils` and no longer uses Mbed TLS.** `sha256_hex`, `sha256_file` and `parse_sha256_checksum` keep their signatures and now carry `HTTPP_UTILS_API`. The hash is a plain FIPS 180-4 implementation (`sha256_state` in `utils.cpp`, the constants are the fractional parts of the cube and square roots of the first primes and were generated, not typed). It had to be rewritten because `psa_hash_*` needs the TLS library, which ADR-0086 keeps out of `httpp_utils`, and because `psa_crypto_init()` state is shared with the TLS code, so a second copy in another shared library would split it. `httpp/sha256.hpp` stays as a header that only includes `utils.hpp`, because `sha256.hpp` was released; the Cython binding now includes `utils.hpp`. The C++/Python parity test maps `utils.hpp` once: the SHA functions to `httpp.utils`, the four `env` functions to `os.environ` / `os.getenv` (listed in `ALIASES`), `env::scope` to nothing.

## Alternatives considered

- **`httpp::getenv`, `httpp::setenv`, `httpp::unsetenv`, `httpp::environ`:** `getenv`, `setenv` and `unsetenv` are libc names, so under `using namespace httpp` a call is ambiguous or silently picks the other one, and some Windows compatibility headers define `setenv` as a macro. `environ` is a global variable on POSIX and a macro (`_environ`) on MSVC, so the name cannot be used at all. One namespace also keeps `get`, `set`, `first` and `scope` together, as `path::` and `temp_path` do, and reads like `os.environ` / `os.getenv`.
- **Header-only:** the Windows branch needs `windows.h`, and the lock has to be one per process (decision 3).
- **SHA-256 stays in core, `env` in utils (two files):** the first version. It kept Mbed TLS's hash, but a program that only hashes files had to carry the HTTP and TLS stack, and the owner preferred one place for the small helpers.
- **A hand-written SHA-256 versus a vendored one** (for example a public-domain single-file implementation): the algorithm is about 60 lines and the constants are derived, so the code can be checked against hashlib; a vendored file would add a licence note and a third-party directory for the same lines.
- **SHA-256 stays in core, `env` in utils (two files):** the first version. It kept Mbed TLS's hash, but a program that only hashes files had to carry the HTTP and TLS stack, and the owner preferred one place for the small helpers.
- **A hand-written SHA-256 versus a vendored one** (for example a public-domain single-file implementation): the algorithm is about 60 lines and the constants are derived, so the code can be checked against hashlib; a vendored file would add a licence note and a third-party directory for the same lines.
- **A class `env` with static members, like `path::cwd()`:** nothing here has state to hold; a namespace says that.
- **Making the lock cover everyone:** impossible. Only `getenv` calls made through httpp take it.

## Consequences

- Nothing that worked changes meaning; the proxy tests, the logging test and the CLI behave as before. A machine that had proxy variables set keeps them after the proxy tests (they were lost before).
- The lock does not make the C library safe: set variables before starting threads (or in a test), and read them through `get`. `env.hpp` says so.
- On Windows an empty value cannot be kept (`set(name, "")` removes the variable, so `get` says `nullopt` where POSIX says `""`). `first` treats empty as not set everywhere, so nothing in httpp depends on the difference.
- Python: not exposed. `os.environ` and `os.getenv` are its side of this, so `tests/python/test_cpp_python_parity.py` maps `env.hpp` (the functions and `scope`) to `None` with that reason.
- SHA-256 is now portable C++ in the library itself. The old comment already said "portable code only" for the Mbed TLS version, so speed is the same class; the upgrade path (SHA-NI on x86, the ARMv8 crypto instructions, picked at run time) is unchanged. It is checked against `hashlib` for the padding edges (55, 56, 63, 64, 65, 119, 120, 127, 128, 129 bytes), a million `a`, and files of 0 to 200000 bytes around the 64-byte block and the 64 KiB read chunk.
- `httpp_core` no longer has `sha256.cpp`; `download.cpp` reads `utils.hpp`. A program that only hashes files links `httpp_utils` and nothing else.
- Not run here: Windows and macOS.

## Taken by default (for the owner to confirm)

- `get` keeps "set and empty" apart from "not set" on POSIX; `first` skips both.
- `scope(name)` with no value means "remove it while the scope lives".
- `set` and `unset` return `bool` and never throw; `get` of a name that cannot be a variable (empty, or with `=`) is `nullopt`.
