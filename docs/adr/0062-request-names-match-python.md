# ADR-0062: The C++ request names match Python's: `httpp::request` is the client builder, `httpp::server_request` is the handler's request

- **Status:** accepted (rename done and checked on Linux, see Consequences)
- **Decided by:** the project owner ("Python or C++ or both must change, but be identical as far as possible"; the names were left to the assistant)
- **Source:** `include/httpp/client.hpp`, `include/httpp/server.hpp`, `src/bindings/python/httpp/__init__.py`, `tests/python/test_cpp_python_parity.py`, ADR-0016

## Context

Python names the two requests by role: `httpp.Request` is the client's builder and `httpp.ServerRequest` is what a server handler receives. C++ had the opposite: `httpp::request` was the handler's request and the builder was nested as `httpp::client::request`, to avoid that clash. The same word meant different things in the two languages, and every page that shows both had to explain it.

## Decision

- `httpp::request` is the client builder (`httpp::request(url).method("POST").run()`). It is a class of its own in `client.hpp`, not a member of `client`.
- `httpp::server_request` is the request a server handler receives (`server.hpp`), the counterpart of Python's `httpp.ServerRequest`.
- `httpp::client::request` is removed, with no alias. A second name for the class is the thing this change removes, and while the major version is 0 a minor release may change an API (listed under **Changed**).
- Python is unchanged: its names already follow this split.
- `httpp::curl::request` stays as it is. It lives in its own namespace and the parity test maps it to `httpp.Request`.

## Rejected alternatives

- **Rename the Python side** (`Request` for the handler's request, `ClientRequest` for the builder). It would break far more user code than the C++ side (every route that names `request`) and Python's `Request` is also the shape other frameworks use for the client.
- **Keep `client::request` as an alias for a release.** Two names for one class, in a library that is still 0.x.

## Consequences

- Every C++ handler spells its first parameter `const httpp::server_request&`; code that built requests with `httpp::client::request` writes `httpp::request`. Tests, examples, the benchmark and the Cython declarations were updated; ADRs 0051 to 0053 keep their original wording as a record.
- Still different on purpose, to decide later: C++ has one `httpp::response` for both the client's reply and the server's reply, while Python has `Response` and `ResponseHead`.
- Checked on Linux (GCC 13, Python 3.12, a build of this tree): the library, the CLI and the C++ tests compile with no error and 522 of 522 C++ tests pass; the Cython binding builds and the Python suite gives 576 passed, 7 skipped, 0 failed (the parity test included, with the new `server_request` key). Not checked: Windows, macOS, a pip-built wheel, Python 3.14t.
