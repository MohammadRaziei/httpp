# ADR-0017: Python binding policy: speed decides, only public headers, only what is necessary

- **Status:** accepted
- **Date:** 2026-10-19
- **Decided by:** the project owner
- **Source:** `docs/adr/0016-hybrid-api-parity.md`, `src/bindings/python/httpp_cy.pyx`, `tests/python/test_cpp_python_parity.py`

## Context

ADR-0016 set "C++ and Python expose equivalent public APIs" as the goal. Looking at duplicated logic between the two languages
(path templates, MIME types, query encoding, two client engines) raised the question of one rule for "which layer owns the logic".
The owner's answer: there is no single rule, because it depends on the case.

## Decision

1. **Speed decides where logic lives.** For each piece of logic the choice is whichever of C++ or Python gives the fastest result in that
   case. When it is not obvious, measure. There is no blanket "everything in C++" or "everything in Python" rule.
2. **Python binds only what `include/` exports.** The Cython binding may use only the public headers (`include/httpp/*.hpp`, the
   `HTTPP_API` declarations). It must not reach into `src/core/internal/` or any other private header. The C++ glue embedded in the
   `.pyx` files (inbox, trampolines) is Python-specific code and also uses public headers only.
3. **A public C++ function gets a Python binding only if it is necessary.** If it is not necessary, Python simply does not carry it
   (Python's standard library or Python code may already cover it, or Python users never need it). Every public C++ name is therefore
   either bound, or listed in `NOT_EXPOSED` in `tests/python/test_cpp_python_parity.py` with the reason. Adding a C++ name forces
   that decision to be made and written down.
4. **Python may do work that C++ does not.** Some logic exists only because of Python (the GIL, the asyncio loop bridge, hooks run through an
   executor, type-hint based validation). It does not have to exist in C++.

## Consequences

- ADR-0016's goal "parity is the goal" is **replaced** by rules 2 and 3. Its comparison and its list of differences stay as an inventory.
- `tests/python/test_cpp_python_parity.py` keeps its job as a forcing function, but its list is now `NOT_EXPOSED` (decisions), not `KNOWN_GAPS`,
  and a new test checks rule 2 (the `.pyx` files include only `httpp/*.hpp` headers).
- Duplicated logic is judged by rule 1, not removed by default. First applications: path templates now have one parser (registration time, so
  no speed question); query encoding in `Client` moved to the public C++ `url::build_query` (4x faster, identical output); the duplicated
  client option block was merged. Still open: MIME lookup in `file()` (Python `mimetypes` and the C++ table disagree for some extensions; the C++
  lookup is not public) and the sync versus async client engines (a separate decision).
- Decided by the owner for the bindings added before this ADR: `sha256_hex`, `sha256_file` and `parse_sha256_checksum` **stay**, because
  binding them lets Python check the C++ code against `hashlib` (a benefit of having a binding), and the C++ code is as fast as `hashlib`
  anyway. They live in **`httpp.utils`**, not in the main `httpp` namespace. `Client(keep_alive=...)` is still waiting for a decision (see `checklist.md`).
- Free functions that Python carries go into a submodule (`httpp.utils`, like `httpp.progress`), not into the main namespace.

## Related

ADR-0001, ADR-0002, ADR-0016
