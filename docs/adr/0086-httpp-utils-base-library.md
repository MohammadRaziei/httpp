# ADR-0086: `httpp_utils`, a base library below `httpp_core` (logging, path, sformat, progress)

- **Status:** accepted
- **Decided by:** the project owner (name `httpp_utils`, members log + path + format + progress; the core library file is `libhttpp`)
- **Source:** `CMakeLists.txt`, `include/httpp/export.hpp`, `src/utils/`, `src/bindings/python/httpp_utils_python.pyx`, `src/bindings/python/CMakeLists.txt`, `tests/utils/`

## Context

libspeech logs everywhere through `httpp::logging` and `httpp::sformat` (ADR-0057, ADR-0080). Both lived in `libhttpp_core`, so a program that only wants to log had to link and ship the HTTP stack (about 3 MB with mbedtls, plus `liburlparser`, linked PUBLIC). `logging.cpp`, `path.cpp` and `progress.cpp` need only the standard library, OS headers and the vendored header-only `indicators`; `sformat` is header-only.

## Decision

- **A new shared library `httpp_utils`** (`libhttpp_utils.so`, `.dylib`, `httpp_utils.dll`; alias `httpp::utils`; exported as `httpp::httpp_utils`) built from `src/utils/`: logging (with `sformat`), path, progress. `httpp_core` links it PUBLIC, so everything that links `httpp::core` keeps working unchanged.
- **Rule of membership.** A source belongs to `httpp_utils` only if it needs nothing but the standard library, the operating system and header-only code: no `httpp_core`, TLS, asio, urlparser or curl. The test `utils_needed` enforces it (`readelf -d` / `otool -L`).
- **Shared, never static or header-only: the one-registry rule.** The logger registry (`get_logger("libspeech.dsp")`) must exist once per process. With two copies, a logger made by a library and the handler the Python bridge installs would sit in different registries and records would be lost. (This is the reason for *shared*; it says nothing against adding a pure-utility source later.) Test: `utils_one_registry`.
- **Two export macros.** `HTTPP_UTILS_API` (`HTTPP_BUILDING_UTILS_DLL`) for `logging.hpp`, `path.hpp`, `progress.hpp`; `HTTPP_API` stays for the core. On Windows a symbol must be `dllexport` in the DLL that defines it and `dllimport` everywhere else, which one macro for two DLLs cannot give.
- **The library's own log threshold is read inline.** The core's `HTTPP_LOG` macro used to call an exported function; with the split that would be a call across two libraries for every disabled line. `detail::own_log_level` is an exported, constant-initialized `std::atomic<int>` that `log_enabled` reads inline (one relaxed load), so a disabled line costs the same as before. The public `logger::debug(...)` family is unchanged (still out-of-line).
- **File names.** The core library is now `libhttpp.so` / `libhttpp.dylib` / `httpp.dll` (the CMake target stays `httpp_core`, so `httpp::httpp_core` and `httpp::core` still work); `httpp.pdb` stays with `httpp.exe` (`PDB_NAME httpp_lib`). Both libraries sit in the same folder with RPATH `$ORIGIN` / `@loader_path`.
- **Python.** The log bridge moves out of the extension into `httpp_utils_python` (links only `libhttpp_utils`): `set_log_level`, `set_log_file`, `set_log_system`, `set_log_callback`, `_bridge_logger`, `_unbridge_logger`, `_set_logger_level`, `_log_from_cpp`. The extension `httpp_cy` is renamed `httpp_python`. The public Python API does not change. `httpp/__init__.py` still imports `httpp_python` eagerly, so the Python split is a cleaner layout today; the logging part can be imported alone as `httpp.httpp_utils_python`.

## Rejected alternatives

- **A hook in libspeech's DSP** (a function pointer the models library connects to httpp): two log paths again, and a DSP-only user gets no log.
- **A static `httpp_utils`:** breaks the one-registry rule.
- **A separate repository or wheel:** the same split with more release work.
- **`progress` in the core:** it needs only `indicators`, and it is a utility a long-running program (libspeech) may want without the HTTP stack.

## Consequences

- A program that links only `httpp::utils` ships a library of about 180 KB (Linux, Release) that needs only libstdc++, libgcc and libc.
- Consumers that load the DLL by hand (libspeech's `_httpp_dir.py` on Windows) must find both `httpp.dll` and `httpp_utils.dll`.
- Not verified here: Windows (the export macros), macOS, auditwheel/delocate repair. They need the CI jobs.
- Revisit: a public inline fast path for `logger::debug(...)` (skip the call and the argument evaluation when disabled) is a separate API decision, not part of this split.
