# ADR-0095: a named "no limit" for `max_redirects`, `max_time` and `max_response_size`

- **Status:** proposed; not implemented (what the type is, and how Python spells it, are not decided)
- **Decided by:** nobody yet for the type; the owner asked for a named value instead of a number and floated a `std::optional`-like type
- **Source:** `include/httpp/client.hpp`, `src/core/client.cpp` (`redirect_hops`), `src/core/internal/http_exchange.hpp`, `src/bindings/python/cy/client.pxi` (`_apply_client_options`), `src/bindings/python/cy/support.pxi`
- **Follows:** ADR-0079, 0092 and 0093

## Context

Three settings have a "no limit", and each spells it differently:

| Setting | No limit | Other special values |
|---|---|---|
| `max_redirects(int)` | `-1` | `0` follows none; any other negative number throws `std::invalid_argument` (ADR-0079) |
| `max_time(long)` | `0` | a negative number throws |
| `max_response_size(size_t)` (on `request`) | `0` | none |

A bare number is easy to misuse, and a shared constant such as `-1` would be wrong for two of the three. A getter that has to return a number cannot say "no limit" without one of those encodings. In Python, `None` already means "the argument was not given" (`if max_time is not None:` in `_apply_client_options`), so `None` cannot also mean "no limit".

## Decision (proposed)

1. A named value, `httpp::unlimited`, and a type that holds either a number or "unlimited". The setters and the getters of the three settings use it, so they stay the same name with the same type (ADR-0092):
   ```cpp
   c.max_redirects(httpp::unlimited);
   c.max_redirects(5);
   limit<int> r = c.max_redirects();       // reads back without a magic number
   ```
2. The encodings inside `session_settings` and the engine do not change (`-1` and `0` stay as they are); the conversion happens at the API boundary.
3. `max_redirects(-1)` keeps working, because ADR-0079 documents it.
4. Settings that have a "default" meaning (`timeout`, `connect_timeout`) do not take it; they stay plain numbers.

## Still open: what the type is

- **(a) An own class `limit<T>`** (about ten lines) holding an optional value, with implicit construction from `T` and from `unlimited_t`, and **no conversion from `std::optional<T>`**. An empty optional that arrives by mistake (a missing config key, `max_time({})`) does not compile, so "no value" can never silently become "no limit".
- **(b) An alias:** `template <class T> using limit = std::optional<T>;` with `unlimited_t = std::nullopt_t`. It is the smaller change and `std::optional` is already used in the bindings (`optional[cpp_url]` in `support.pxi`, Cython 3.1 or newer). The cost: it is the same type as `std::optional`, so any empty optional passed in means "no limit", and `max_redirects({})` does too. For a limit, absent meaning unlimited is the unsafe direction (unbounded redirects, time or response size).

The maintainer's note in discussion was that (b) is simpler and easier to bind; the point against it is the one above. Both are workable.

## Still open: Python

`None` cannot be the spelling of "no limit" while it means "not given". Options: `math.inf`, or `0` for `max_time` as today, or a module-level `httpp.unlimited`. This does not depend on the C++ type.

## Rejected alternatives

- **One shared number constant** (for example `httpp::nolimit = -1`): the three settings encode it differently, so it would be wrong for `max_time` and `max_response_size`.
- **Only overloads that take a tag** (`max_time(unlimited_t)`) and keep returning a number from the getter: it removes the magic number from the setter but not from the getter.

## Consequences

- A small public vocabulary type, and about three more declarations in the mixin of ADR-0093 (written once for all classes).
- The Cython declarations list the new type; with (a) they declare the parameter as the plain number and let the C++ compiler do the implicit conversion, plus an overload for the tag. Not tried.
- Independent of ADR-0093: it can be done before or after the mixin.

## Not verified

Nothing was compiled. The Cython declaration trick above is an expectation, not a result.

## Related

ADR-0079, 0092 and 0093
