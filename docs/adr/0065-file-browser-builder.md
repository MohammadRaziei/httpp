# ADR-0067: The file browser is a builder class with enums

- **Status:** accepted; amends ADR-0032
- **Decided by:** the project owner ("this is not principled: it should be OOP, `file_browser(...).set_...().mount()`, and the view must be an enum"); the details are Claude's
- **Source:** `include/httpp/file_browser.hpp`, `src/core/file_browser.cpp`, `src/cli/httpp_cli.cpp`, `src/bindings/python/cy/server.pxi`, `tests/cpp/test_file_browser.cpp`

## Context

ADR-0032 shipped the browser as a free function `mount_file_browser(server&, base, root, file_browser_options)`. The options were a plain struct whose theme, view and sort were strings ("auto", "grid", "size"): a typo compiled, was checked only when mounting, and every other part of the library (`server::set_*`, ADR-0055 `path`) is a class with chained setters instead.

## Decision

- `httpp::file_browser(root)` with chained setters `set_title`, `set_theme`, `set_accent`, `set_search`, `set_show_hidden`, `set_max_entries`, `set_view`, `set_sort`, and `mount(server&, base = "/") const`.
- `theme_mode { system, light, dark }`, `view_mode { list, grid }`, `sort_key { name, size, date }` are `enum class` members of `file_browser`. `system` is the old "auto" (`auto` is a keyword); the page still receives the string "auto", so the HTML is unchanged.
- Values are checked where they are given: `set_accent` (`#rrggbb` or empty) and `set_max_entries` (above 0) throw `std::invalid_argument` at the setter and leave the browser unchanged. `mount` checks what depends on the world: `base` starts with '/', `root` is a folder, `root` is not the root of the file system. The root is not checked in the constructor, because the folder may be created before mounting.
- `file_browser_options` and `mount_file_browser` are removed, not kept as deprecated aliases (version 0.x, as `httpp::fs` was removed in ADR-0058). Listed as a change in the CHANGELOG.
- Python mirrors it: `httpp.FileBrowser(root, **keywords)` with the same chained `set_*` methods (each returns the browser) and `mount(server, base="/")`; the enums are `FileBrowser.Theme` (`SYSTEM`, `LIGHT`, `DARK`), `FileBrowser.View` (`LIST`, `GRID`) and `FileBrowser.Sort` (`NAME`, `SIZE`, `DATE`), real `enum.Enum` types, and an enum setter given a string or another enum raises `TypeError`. Constructor keywords are the setters without `set_`, so both forms exist as on `Server` and the clients. `Server.mount_file_browser(base, root, **options)` is removed: one way to do it in both languages. The numbers behind the Python enums are checked against the C++ enums by `static_assert` in `cy/support.pxi`, so reordering a C++ enum breaks the build instead of the meaning. The CLI (`httpp browse`) keeps its `--theme/--view/--sort` choices and maps them to the enums.
- Export: the member functions carry `HTTPP_API`, not the class, so the private `std::string` members need no DLL interface (ADR-0057 exported classes because they had to be derived from).

## Consequences

- A wrong value cannot be written in C++ for theme, view and sort (it does not compile); a bad accent or `max_entries` fails at the line that sets it.
- Breaking for users of 0.8.0 and 0.8.1: C++ `httpp::mount_file_browser(s, "/files", root, options)` becomes `httpp::file_browser(root).set_...().mount(s, "/files")`; Python `app.mount_file_browser("/files", root, theme="dark")` becomes `FileBrowser(root, theme=FileBrowser.Theme.DARK).mount(app, "/files")`. The CLI is unchanged.
- The browser object can be mounted more than once.

## Verification

Sandbox, Linux, Release. C++ suite 524 passed (16 of them the file browser; three new: the setters chain, a refused value changes nothing, the defaults reach the page). Python 3.14t, wheel built from the final tree: 868 passed, 4 skipped, including `test_file_browser.py` (rewritten: enums, keywords, chaining, mounting twice, a bad value at the setter changes nothing, `TypeError` for a string where an enum is expected, `Server.mount_file_browser` gone), the parity test (the new class is mapped to `httpp.FileBrowser` and every C++ setter has a Python method) and the CLI tests.
Not verified: sanitizers on the new code, Windows (MSVC and the method-level `HTTPP_API`), macOS, Python 3.13 and 3.14 with a GIL for this change.
