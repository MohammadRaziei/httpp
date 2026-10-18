# ADR-0014: Coding conventions: STL-style `snake_case`, the "ponytail" style

- **Status:** accepted
- **Decided by:** the project owner
- **Source:** `handoff.md` section 1

## Decision

- C++ naming follows the STL (`snake_case` for types, functions, variables and enumerators).
- TDD: the test comes before (or together with) the change.
- The [ponytail](https://github.com/DietrichGebert/ponytail) style: the simplest working solution, reuse of the stdlib and existing code,
  no unrequested boilerplate, fix the cause not the symptom. Every deliberate simplification is marked `// ponytail:` or `# ponytail:` with an **upgrade path**.
  Input validation, error handling and security stay complete (if doing it right takes more work, do the work).
- A critical architectural decision is raised with the project owner before it is carried out; non-critical decisions are made without asking.
- Documents carry no numbered priority labels (P0/P1/P2); priority is stated in plain words.
- **Everything written in the repository is English** (code, comments, docs, ADRs, changelog).
- **The em dash character is not used anywhere in the repository.** Use a colon, a comma or a hyphen, or rewrite the sentence.
- `httplib.h` stays untouched; an ASGI/WSGI adapter is not part of the plan.

## Consequences

Every `ponytail:` in the code is an explicit debt; they can be listed with `grep -rn "ponytail:" src include`.
