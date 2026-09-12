# third_party

Vendored dependencies. Each is either a committed file (`cpp-httplib`,
`indicators`) or a git submodule built from source (`mbedtls`,
`liburlparser`) — never a system package.

## indicators (not wired up yet)

`indicators/indicators.hpp` — a single-header terminal progress-bar/spinner
library (github.com/p-ranav/indicators). Vendored for later use (e.g. a
progress bar for `httpp download`), not currently included or built by
anything.

## Local patches

- **liburlparser/CMakeLists.txt**: two `if(DEFINED SKBUILD)` checks were
  narrowed to `if(DEFINED SKBUILD AND SKBUILD_PROJECT_NAME STREQUAL PROJECT_NAME)`.
  Upstream assumes it's always the top-level scikit-build-core package, so
  as soon as `SKBUILD` is defined it tries to build its own nanobind Python
  bindings (and otherwise `RETURN()`s before ever defining the `url::base`
  target). That assumption breaks when liburlparser is vendored as a
  subdirectory of another scikit-build-core project (httpp): `SKBUILD` is
  defined for *httpp's* install, not liburlparser's, so the guard now also
  checks that liburlparser is genuinely the package being installed.
