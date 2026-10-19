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

---

## D-011 — Scope includes kernel-dependent workloads; system-mode is not ruled out

**Date:** 2026-10-19 · **Status:** accepted

**Supersedes:** the Phase 0 assumption that "system-mode or kernel emulation"
is an explicit non-goal. That assumption is withdrawn.

**Context.** An early scoping choice limited HYPERION to user-mode translation
and treated kernel/system-mode emulation as out of scope. That was made before
any measurement of what the reference device (unprivileged, no KVM, ARM64
Android) can actually do. It silently capped the project's compatibility
ceiling: a user-mode translator cannot provide `systemd` as PID 1, containers,
or other kernel-dependent behaviour, no matter how good its instruction
coverage becomes.

The project's stated goal is a broadly compatible Linux execution environment,
not merely an instruction translator. That goal requires deciding what to do
about kernel semantics rather than assuming they are unreachable.

**Decision.** Kernel-level compatibility is **in scope for investigation**, not
assumed impossible and not assumed solved. Concretely:

1. The **user-mode translator remains the primary path** and the focus of
   Phases 2–5. It has demonstrated feasibility on Android and the highest
   performance ceiling.
2. The **syscall layer must be abstracted behind an interface** so that syscalls
   can later be directed either to the host kernel or to a user-space kernel
   implementation. This is a design constraint on Phase 6, not a feature.
3. **System-mode and kernel-as-a-process approaches are evaluated, not
   implemented**, until a target-device question is answered (see D-012).

**Consequences.** The design space is wider and the risk is higher. This is
accepted because the narrower scope could not meet the actual goal. The
evaluation and its measured evidence live in
[`docs/architecture-feasibility.md`](docs/architecture-feasibility.md).
Phases 2–6 are unaffected in ordering; Phase 6 gains an interface requirement.

---

## D-012 — Kernel-as-a-process (UML) is a candidate compatibility tier, pending AArch64 validation

**Date:** 2026-10-19 · **Status:** provisional

**Context.** A user-mode translator cannot provide real kernel semantics. One
mechanism can: run a real Linux kernel as an unprivileged user-space process
(User-Mode Linux). This was **measured**, not assumed.

Measured on 2026-10-19, x86-64 Linux host, uid 10001, no root, no KVM:

- UML booted kernel `6.12.101` from an isolated ext4 image (`/dev/ubda`), with
  the host root confirmed *not* visible inside the guest.
- `systemd 257.13-1~deb13u1` ran as PID 1 and reached `multi-user.target` and
  `graphical.target`; `dbus`, `systemd-logind`, and `getty@tty1` started.
- All namespace types (`user`, `mount`, `pid`, `uts`, `ipc`, `net`) worked
  inside the guest.
- Guest was single-CPU.

**Decision.** Treat kernel-as-a-process as a **candidate compatibility tier**,
and record it as *provisional* rather than adopted, because the decisive
question is unanswered: **whether a maintainable AArch64 UML exists and runs
under Android 10's kernel.** All evidence above is from an x86-64 host.

No kernel-as-a-process code will be built before that question is answered by
measurement on the target device.

**Consequences.** If AArch64 UML works on the target, HYPERION can plausibly
offer a compatibility tier for kernel-dependent workloads while keeping the
fast user-mode path for ordinary programs. If it does not, kernel-dependent
workloads remain out of reach without root, and that limitation must be stated
plainly in `COMPATIBILITY.md`. Either way, the user-mode path is unaffected.

**Risks.** Android's seccomp and SELinux policies may block the `ptrace` and
`mmap` behaviour UML requires. Performance of the syscall boundary is
unmeasured. See the risk table in the feasibility document.

---

## D-013 — Build the core; reuse the oracles and the designs

**Date:** 2026-10-19 · **Status:** accepted

**Context.** Phase 1 required a prior-art survey before any engine code, so that
HYPERION does not reimplement a solved problem, and does not vendor code it
cannot legally or practically use. The survey is
[`docs/prior-art.md`](docs/prior-art.md); all licence claims in it were verified
against upstream licence files.

The finding that forces this decision: **FEX-Emu already implements almost
exactly the pipeline this project's roadmap describes** — x86-64 decoding, a
custom IR, an interpreter, an AArch64 JIT, a code cache with a disk tier, a
syscall layer and library thunks — under the permissive MIT licence, on the
target architecture, and in production use. Box64 covers the same ground with a
different technique (direct dynarec, no IR).

**Decision.**

1. **Build** the decoder, IR, reference interpreter, AArch64 code generator and
   code cache. These are the stated research object; reusing them would remove
   the project's reason to exist.
2. **Reuse as external processes, never linked**: `qemu-x86_64` and
   `qemu-aarch64` (differential-testing oracles), Zydis (independent decoder
   opinion), LLVM/binutils tooling (object inspection). This keeps GPL-2.0 QEMU
   out of the binary, per `THIRD_PARTY.md`.
3. **Reuse as design references, read but not vendored**: FEX's IR and
   `ThunkLibs` design, Box64's dynarec structure, dynarmic's AArch64 emitter.
4. **Defer the library-forwarding decision to Phase 9**, where a proven design
   is worth more than novelty.
5. **State the positioning honestly**: HYPERION is an experimental research
   platform for measuring translation and optimisation techniques. It is not a
   FEX competitor, and users who want to run software should use FEX.

**Consequences.** The project must not drift into "a worse FEX". The measurable
value is the comparison of design choices and the AI-driven optimisation loop
(Phase 8), not raw compatibility. Early phases stay small because nothing large
is being vendored. The cost is that correctness oracles must be wired up
deliberately rather than inherited.

**Supersedes:** nothing. This is the Phase 1 reuse-vs-build boundary required by
`ROADMAP.md` and anticipated by D-002.
