# DECISIONS

Architectural decisions with their reasoning. Format: context → decision →
consequences. Decisions are append-only; superseded entries are marked, not
deleted.

---

## D-001 — Documentation-as-memory, one file per concern

**Date:** 2026-10-19 · **Status:** accepted

**Context.** The project spans many sessions and multiple AI agents. Hidden
conversational memory is not a reliable store. A single README that doubles as
a changelog becomes unusable at scale.

**Decision.** Keep `README.md` as a stable orientation document, `STATUS.md`
for detailed current progress, `ROADMAP.md` for the full staged plan, and a
dedicated file per concern (`ARCHITECTURE.md`, `DECISIONS.md`,
`COMPATIBILITY.md`, `BENCHMARKS.md`, `TESTING.md`, `SECURITY.md`, ...).

**Consequences.** More files, but each stays small and stable. Every milestone
updates `STATUS.md` and `docs/agent-handoff.md`. The README does not grow into
a changelog.

---

## D-002 — No third-party code vendored until the licence/reuse analysis exists

**Date:** 2026-10-19 · **Status:** accepted

**Context.** Reusing mature components (FEX, Box64, QEMU TCG) may be far safer
than reinventing complex subsystems, but silently vendoring code with
incompatible licences or unclear provenance is a real risk.

**Decision.** Phase 1 produces a written comparison and a licence inventory in
`THIRD_PARTY.md` *before* any third-party code is introduced. Reuse decisions
are recorded here.

**Consequences.** Slightly slower start; no licence or provenance surprises.
A future agent cannot accidentally import code without documenting it.

---

## D-003 — Flags: eager first, lazy later, always differential-tested

**Date:** 2026-10-19 · **Status:** accepted

**Context.** x86-64 arithmetic flags (CF, PF, AF, ZF, SF, OF) do not map
cleanly onto AArch64's NZCV. Correctness is easy to get subtly wrong.

**Decision.** Phase 2 implements flags *eagerly* (compute the full flag word
after each flag-setting instruction) for obvious correctness. Lazy flag
materialization is a Phase 5 optimization, introduced only with differential
tests proving equivalence with the eager implementation.

**Consequences.** Phase 2 is slower but trustworthy and serves as the oracle.
Optimization risk is isolated to Phase 5 behind tests.

---

## D-004 — IR between decoder and code generator

**Date:** 2026-10-19 · **Status:** accepted (implementation Phase 2+)

**Context.** Direct x86→AArch64 translation couples the decoder to the host
ISA and makes retargeting and optimization harder.

**Decision.** Introduce an explicit, typed intermediate representation. The
decoder never references AArch64; the code generator never references x86-64.

**Consequences.** An extra design layer to maintain, but a clean boundary for
optimization (Phase 5) and for the reference interpreter (Phase 2), which
executes the same IR the code generator consumes.

---

## D-005 — C++20, CMake, no mandatory third-party runtime dependency

**Date:** 2026-10-19 · **Status:** accepted

**Context.** The target device is Android 10 with no root and no package
manager. The core translator must build and run there without a dependency
ecosystem.

**Decision.** C++20, CMake ≥ 3.20, and a core library with **no** third-party
runtime dependency. Tooling (clang-format, clang-tidy, qemu-user) is
development-only.

**Consequences.** Portable to constrained targets. We forgo convenient
libraries for the core; tests use a small in-repo harness.

---

## D-006 — In-repo, dependency-free test harness

**Date:** 2026-10-19 · **Status:** accepted

**Context.** GoogleTest/Catch2 are convenient but add a dependency to the
target build and to every CI job. A translator needs many small,
table-driven and differential tests.

**Decision.** Implement a minimal harness (`tests/harness/test_harness.hpp`)
with test registration, `CHECK`/`CHECK_EQ`, filtering and CTest integration.
Revisit only if the harness becomes a bottleneck.

**Consequences.** No external dependency; trivial to run under `qemu-user`.
We give up some advanced features (mocking, parameterized fixtures) we do not
currently need.

---

## D-007 — Cross-compile + qemu-user as the standing AArch64 validation path

**Date:** 2026-10-19 · **Status:** accepted

**Context.** The reference target is AArch64, but development hosts are often
x86-64. Generated AArch64 code cannot be executed natively there.

**Decision.** Make `aarch64-linux-gnu-g++` + `qemu-aarch64` a first-class,
CI-enforced validation path, exercised from Phase 0 so it is proven before any
code generator exists. Static linking keeps the artifacts self-contained.

**Consequences.** AArch64 codegen can be developed and regression-tested on
x86-64 hosts. qemu-user is not cycle-accurate, so it validates *correctness*,
never performance; performance claims require real AArch64 hardware.

---

## D-008 — Host capabilities are probed, never assumed

**Date:** 2026-10-19 · **Status:** accepted

**Context.** Page size, cache-line size and W^X policy differ across hosts and
affect code-cache design and correctness.

**Decision.** A `host_probe` component measures these at runtime and reports
them; benchmark and bug reports include the measured fingerprint.

**Consequences.** Code adapts to the real host. Probe results are included in
the benchmark JSON so measurements are attributable to an environment.

---

## D-009 — Benchmark reports may never contain fabricated numbers

**Date:** 2026-10-19 · **Status:** accepted

**Context.** Performance claims are easy to inflate and hard to verify.

**Decision.** Benchmark output uses explicit `available: false` markers with a
reason for any metric that cannot be measured yet. `BENCHMARKS.md` states
"unavailable" rather than guessing. Every result records host, compiler, build
configuration and methodology.

**Consequences.** Early reports look sparse, which is honest. No result can be
mistaken for a measurement that was not taken.

---

## D-010 — W^X assumed on AArch64

**Date:** 2026-10-19 · **Status:** accepted

**Context.** Modern AArch64 Linux enforces W^X; a page cannot be writable and
executable simultaneously. Code caches must be written then made executable.

**Decision.** The runtime writes code into `PROT_WRITE` memory, switches to
`PROT_READ|PROT_EXEC`, and performs explicit instruction-cache maintenance
(`__builtin___clear_cache`). No design may assume self-modifying executable
pages.

**Consequences.** Slightly more bookkeeping per emitted block; correctness on
hardened kernels and on the target device.
