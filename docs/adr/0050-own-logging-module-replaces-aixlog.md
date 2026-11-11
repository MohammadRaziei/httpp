# ADR-0050: An internal logging module, ported from libstb, replaces AixLog

- **Status:** accepted (supersedes the logging back end chosen in ADR-0003); the module was made public by ADR-0057
- **Source:** `src/core/internal/logging.hpp`, `src/core/logging.cpp`, `src/core/log.cpp`, `tests/cpp/test_log.cpp`

## Context

httpp wrapped a vendored single-header AixLog (v1.5.0) carrying one patch (upstream redirects `std::clog` for the whole process), reached only through a private ostream, and used
about 10 percent of it: a level, a file, a callback and stderr. libstb, the same owner's library, already has a small logging module shaped like Python's `logging`, with no
dependency, thread-safe handlers, a formatter and a last-resort stderr output.

## Decision

The module is copied into httpp as an internal (not installed, not exported) header and source under `httpp::logging`. Changes from libstb: namespace, a `trace` level (5), a `where`
field on the record (`%(funcName)s`) and `logger::log_from` for a message that is already built, the `stb::` helpers (`sformat`) copied in, `std::runtime_error` instead of `stb::io_error`,
and no `LIBSTB_LOG` environment variable. `src/core/log.cpp` keeps the public API of `httpp/log.hpp` (C++, Cython and Python unchanged) on top of one non-propagating logger named
`httpp`: `set_log_level` is one atomic (a disabled level still builds no message), `set_log_file` and `set_log_callback` swap the handler under a mutex. The vendored AixLog, its
patch and its include path are gone. Nothing of the module is public, so the dual-API rule is untouched; making it public later would need Python bindings.

## Amendment 2026-11-10

`file_handler` takes a UTF-8 `std::string` (opened through `httpp::path::native`, ADR-0054, 0058), not `std::filesystem::path`: the macOS wheels target 10.13 and the macOS CI build failed on it. libstb's core logging files have not changed since the port
(2026-11-05); what libstb added since is the operating-system handlers (`logging_os`) and the Python bridge hardening, of which httpp took the shutdown part.

## Consequences

About 800 lines of our own code replace a 3000-line vendored header and a patch to carry on every upgrade. The default line changes to `time [LEVEL] httpp (function): message`.
The two libraries now hold copies of the same module; if it is ever extracted into a shared package, both switch to it.
