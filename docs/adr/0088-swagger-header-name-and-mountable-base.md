# ADR-0088: `swagger.hpp`, and one base class for what is mounted on a server

- **Status:** accepted
- **Decided by:** the project owner ("`swagger.hpp` instead of `swagger_ui.hpp`" and "a common interface that swagger, file browser and directory server inherit")
- **Source:** `include/httpp/swagger.hpp`, `include/httpp/server.hpp` (`mountable`, `directory`), `include/httpp/file_browser.hpp`, `src/core/swagger.cpp`, `tests/cpp/test_mountable.cpp`
- **Related:** ADR-0066 (option structs become builders), ADR-0081 (`server` is a shared handle), ADR-0082 (`swagger(openapi_url)`)

## Context

- The header that holds `class swagger` was called `swagger_ui.hpp`. The class is `swagger`, the namespace has no "ui" in it, and the page it serves is the only thing it can mean.
- `directory`, `file_browser` and `swagger` are built the same way: a value you configure with `set_*()` calls, then `server mount(server s, const std::string& base = ...) const`. Nothing let code treat them as one kind of thing.

## Decision

- `swagger_ui.hpp` becomes `swagger.hpp` (and `src/core/swagger_ui.cpp` becomes `swagger.cpp`, `cy/swagger_ui.pxi` becomes `cy/swagger.pxi`). The names inside stay: `swagger_ui_asset` and `find_swagger_ui_asset` are about the Swagger UI files and say so. The old header is **not** kept as a forwarding file, as with the names ADR-0082 removed: the project is before 1.0 and one line fixes an include.
- `httpp::mountable`, declared in `server.hpp` just before `directory`, has `virtual server mount(server s, const std::string& base) const = 0` and a virtual destructor. `directory`, `file_browser` and `swagger` derive from it publicly and mark their `mount` as `override`.
- The base's `mount` has **no default for `base`**: the defaults differ (`"/docs"` for swagger, `"/"` for the others). Each class keeps its own default in its own declaration, so `swagger(url).mount(s)` works as before and a call through the base names the base.
- The base has protected constructors and copy operations. It cannot be made or sliced on its own, and the three stay ordinary copyable values.

## Rejected alternatives

- **A template or concept instead of a base class.** Cheaper at run time, but code could not keep a list of different parts (`std::vector<std::unique_ptr<mountable>>`) or take one through a function that is not a template.
- **A `server::mount(const mountable&, base)` function.** Not added: nothing needs it yet, and every public function of C++ is a parity decision (ADR-0017). It is one line to add when something does.
- **A forwarding `swagger_ui.hpp`.** See above.
- **Binding `mountable` in Python.** Its only member is not exported (`HTTPP_API` is on the three classes' own `mount`), so the parity test does not see it, and the Python classes (`Directory`, `FileBrowser`, `Swagger`) are unchanged.

## Consequences

- Code that includes `httpp/swagger_ui.hpp` stops compiling until the include is changed.
- The three classes now have a vtable. Their size grows by one pointer; a call through the base is virtual, a call on the derived type is resolved by the compiler as before.
- `mountable` is not used by the library itself yet; only the tests use it as a base (`tests/cpp/test_mountable.cpp`).
- The parity test finds a class with a base clause (`class directory : public mountable {`).
- **The classes are exported as a whole** (`class HTTPP_API swagger : public mountable`, and the same for `mountable`, `directory` and `file_browser`), the way `logging::handler` is, with MSVC's C4251 silenced around them. A first version exported only the members, as the rest of the library does, and failed to link on Linux (`undefined reference to vtable for httpp::swagger`, and the same inside the Python extension): the copy and the destructor that a caller compiles inline need the vtable, and with hidden visibility it is only exported with the class. The members keep their own `HTTPP_API` too, because the parity test finds the public names by it. `export.hpp` says so.
- Not verified: Windows. The class-level export follows the existing `logging::handler` pattern, but it was built with GCC 13 on Linux only; check MSVC before a release.
