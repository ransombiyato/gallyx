# CONTRIBUTING

Conventions and workflow for Project HYPERION. These apply to human and AI
contributors equally.

---

## 1. The repository is the memory

Important knowledge lives in Git, not in a chat session. Before ending any
work session, commit coherent changes and update `STATUS.md` and
`docs/agent-handoff.md`.

---

## 2. Milestone workflow

For every milestone:

1. Read `STATUS.md`, `ROADMAP.md`, `ARCHITECTURE.md`, `DECISIONS.md`,
   `docs/agent-handoff.md`.
2. Pick the smallest meaningful next task.
3. State its objective and acceptance criteria **before** coding.
4. Inspect existing code before modifying it.
5. Implement only that scope.
6. Build and run the relevant tests.
7. Fix failures before declaring completion.
8. Update docs, status and roadmap.
9. Commit the verified result.
10. Push when authorized; record the next step.

A phase is complete only when its acceptance criteria are demonstrated by
evidence. Existing code is not completion.

---

## 3. Engineering integrity

- Correctness outranks speed.
- Never fabricate benchmark results, test successes, or compatibility claims.
- Distinguish clearly between prototype, implemented, tested and planned.
- Mark unavailable measurements as unavailable.
- Unsupported input must fail loudly, never silently produce wrong behaviour.

---

## 4. Coding conventions

- **Language:** C++20, no compiler extensions (`CMAKE_CXX_EXTENSIONS OFF`).
- **Style:** enforced by `.clang-format` (LLVM base, 4-space indent, 100 cols).
- **Warnings:** the build uses `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wcast-qual -Wformat=2 -Wundef -Wnon-virtual-dtor`; CI treats warnings as
  errors.
- **Headers:** public headers in `include/hyperion/`, guarded, `#pragma`-free;
  include what you use; no `using namespace` in headers.
- **Naming:** `snake_case` functions and variables, `CamelCase` types,
  `kConstantName` constants, `snake_case` namespaces, trailing `_` for private
  members.
- **Comments:** explain *why*, not *what*. Document non-obvious invariants,
  workarounds and trade-offs. Do not narrate the diff or restate the code.
- **Errors:** prefer explicit status/`expected`-style returns over exceptions in
  the core; exceptions are acceptable in tests and the CLI.
- **Licence header:** every source file starts with
  `// SPDX-License-Identifier: Apache-2.0` and the copyright line.

### Boundaries that must not be crossed

- The **decoder** must not reference AArch64.
- The **code generator** must not reference x86-64.
- The **interpreter** must not be optimized at the cost of clarity.
- The **CLI** must not contain translation logic.

---

## 5. Commit conventions

- One coherent change per commit; the tree must build and tests must pass.
- Conventional-commit prefixes: `feat:`, `fix:`, `docs:`, `test:`, `build:`,
  `refactor:`, `perf:`, `chore:`.
- Reference the phase and milestone, e.g.
  `feat(decoder): decode mov r64, imm64 (phase 2)`.
- Never commit generated build output, secrets, or large binaries.

---

## 6. Branching and review

- `main` is protected by convention. Develop on a branch
  (`phase-<n>-<short-name>`) and open a pull request.
- CI must be green before merge.
- Pull requests state: objective, acceptance criteria, evidence (commands and
  results), and remaining limitations.

---

## 7. Adding a dependency

Before adding any third-party dependency:

1. Confirm it is genuinely needed (see D-005: the core has none).
2. Record it in `THIRD_PARTY.md` with version and licence.
3. Confirm licence compatibility with Apache-2.0.
4. Justify the choice in `DECISIONS.md`.

No vendored third-party code until the Phase 1 analysis is committed (D-002).

---

## 8. Documentation duties

| Change | Update |
| --- | --- |
| Any milestone | `STATUS.md`, `CHANGELOG.md`, `docs/agent-handoff.md` |
| New/changed design | `ARCHITECTURE.md`, `DECISIONS.md` |
| New instruction/ABI feature | `COMPATIBILITY.md` |
| Performance work | `BENCHMARKS.md` |
| New test/fuzz target | `TESTING.md` |
| New dependency | `THIRD_PARTY.md` |
| New limitation/bug | `docs/known-issues.md` |
