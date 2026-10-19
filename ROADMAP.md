# ROADMAP

Staged plan for Project HYPERION. Each phase has **exit criteria that must be
demonstrated by evidence**, not merely by the existence of code.

Status legend: ✅ complete · 🔄 in progress · ⬜ not started

**Overall: Phase 0 complete, Phase 1 next.**

| Phase | Title | Status |
| --- | --- | --- |
| 0 | Project foundation | ✅ complete |
| 1 | Establish the baseline | ⬜ not started |
| 2 | Minimal instruction execution | ⬜ not started |
| 3 | First native code generation | ⬜ not started |
| 4 | Basic dynamic binary translator | ⬜ not started |
| 5 | Performance measurement and optimization | ⬜ not started |
| 6 | Linux ABI and syscall compatibility | ⬜ not started |
| 7 | Advanced CPU compatibility | ⬜ not started |
| 8 | AI optimization laboratory | ⬜ not started |
| 9 | Real application compatibility | ⬜ not started |
| 10 | Stabilization and release | ⬜ not started |

---

## Scope correction (2026-10-19)

The original Phase 0 plan stated that kernel/system-mode emulation was an
explicit non-goal. **That assumption has been withdrawn.** It was made before
any measurement of the reference device and it capped the achievable
compatibility below the project's actual goal, which is a broadly compatible
Linux execution environment — not merely an instruction translator.

What changed, and what did not:

- **Unchanged:** the user-mode translator is the primary path, and Phases 2-5
  build it. Nothing in the phase order moves.
- **Changed:** kernel-level compatibility is now in scope for *investigation*
  (`DECISIONS.md` D-011, D-012). A measured experiment showed that a real Linux
  kernel can run as an unprivileged process, with `systemd` reaching
  `multi-user.target`; an AArch64 port of that mechanism exists.
- **New constraint:** the Phase 6 syscall layer must sit behind an interface so
  the same translator can direct syscalls either to the host kernel or to a
  user-space kernel. This is a design constraint, not a feature to build now.
- **New risk:** the AArch64 port is unmerged and untested on the reference
  device (R7, R1 in `docs/architecture-feasibility.md`).

Full analysis and evidence: [`docs/architecture-feasibility.md`](docs/architecture-feasibility.md).
No kernel-level code is to be written until the tier is validated on the
reference device.

---

## Phase 0 — Project foundation ✅

**Objective.** A buildable, testable, documented repository that a new agent
can understand without the original conversation.

**Delivered.**

- CMake/C++20 project building clean with `-Werror` under GCC and Clang.
- Core library (`version`, `host_probe`) with public headers.
- Dependency-free test harness, wired into CTest.
- Cross-architecture validation pipeline: AArch64 cross-build executed under
  `qemu-user`, proven working in CI-equivalent local runs.
- Host benchmark harness emitting machine-readable JSON with guest-execution
  metrics explicitly marked unavailable.
- Full documentation set, CI workflow, reproducible bootstrap script.
- Local git history with a coherent milestone commit.

**Exit criteria — evidence.**

| Criterion | Evidence |
| --- | --- |
| Project builds | `scripts/dev.sh build` succeeds; see STATUS.md |
| Tests pass | `scripts/dev.sh test` → 2/2 tests passed |
| Cross-arch pipeline works | `scripts/dev.sh cross-test` → 2/2 passed, AArch64 binary ran under qemu |
| Documentation committed | All files listed in README index present |
| New agent can resume | `docs/agent-handoff.md` describes state and next task |

**Explicitly not done.** No decoder, no IR, no interpreter, no code generator,
no syscall layer.

---

## Phase 1 — Establish the baseline ⬜

**Objective.** Know the prior art and have a reproducible measurement harness
*before* writing an engine.

**Work items.**

1. Investigate FEX-Emu, Box64, QEMU TCG and relevant decoder/compiler projects.
2. Document for each: reuse potential, licence, design trade-offs, comparison
   against a custom implementation.
3. Decide the reuse-vs-build boundary and record it in `DECISIONS.md`.
4. Build a reproducible benchmark harness: record host CPU, OS, compiler,
   build configuration and methodology.
5. Record baseline numbers where obtainable; mark unavailable measurements as
   unavailable — **never fabricate**.
6. **(added by the scope correction, optional)** Cross-build the AArch64 UML
   port and confirm the binary is produced. This is a cheap way to raise
   confidence in the kernel-as-process tier (R1) without committing to it.
   Report only what is measured.

**Exit criteria.**

- `THIRD_PARTY.md` and a baseline comparison document are committed.
- `benchmarks/` produces repeatable, attributable results.
- `BENCHMARKS.md` documents methodology and any measured baselines.

---

## Phase 2 — Minimal instruction execution ⬜

**Objective.** Correctly execute a small, explicitly documented instruction
subset.

**Work items.**

1. Define the subset (start: `mov` reg/imm/mem, `add`, `sub`, `and`, `or`,
   `xor`, `cmp`, `test`, `inc`/`dec`, `jmp`, `jcc`, `call`, `ret`, `push`,
   `pop`, `nop`, `lea`, `syscall` stub) in `COMPATIBILITY.md`.
2. Implement the decoder for that subset.
3. Implement guest state and instruction semantics in the IR/interpreter.
4. Build automated tests for registers, flags, arithmetic, memory, control flow.
5. Differential-test the interpreter against an independent reference
   (a native x86-64 oracle and/or QEMU).

**Exit criteria.** The supported subset passes differential tests against an
independent reference. Unsupported instructions fail explicitly, never silently.

---

## Phase 3 — First native code generation ⬜

**Objective.** Execute the Phase 2 subset through generated AArch64 code.

**Work items.** AArch64 encoder/emitter; executable memory management; cache
maintenance; guest-state marshalling; error reporting. Keep the interpreter as
oracle.

**Exit criteria.** Verified test programs execute correctly through generated
AArch64 code, validated under `qemu-user` on x86-64 hosts and natively on
AArch64 hosts.

---

## Phase 4 — Basic dynamic binary translator ⬜

**Objective.** A working translator loop with a code cache.

**Work items.** Translation blocks, translation cache, guest PC tracking,
block exits, cache invalidation, explicit handling of unsupported instructions.

**Exit criteria.** A repeatable suite of small guest programs runs correctly.

---

## Phase 5 — Performance measurement and optimization ⬜

**Objective.** Measured, reversible optimization.

**Work items.** Baseline profiling; experiment independently with register
allocation, block chaining, faster decoding, lower-overhead dispatch,
instruction selection, constant propagation, dead-code elimination, code-cache
policies, multi-tier compilation. Preserve a known-good implementation; reject
changes failing correctness tests or regressing performance.

**Exit criteria.** Reproducible performance measurements and documented
optimization decisions.

---

## Phase 6 — Linux ABI and syscall compatibility ⬜

**Objective.** Run real Linux guest programs.

**Work items.** Guest syscall layer, ABI-sensitive structures, memory mapping,
file descriptors, signals, threading, futexes. Feature matrix + integration tests.

**Exit criteria.** A clearly defined set of real Linux guest programs works
correctly. No claim of general compatibility.

---

## Phase 7 — Advanced CPU compatibility ⬜

**Objective.** Handle complex instruction families.

**Work items.** SIMD (SSE/AVX subsets), atomics, exceptions, memory ordering,
CPUID, multithreading, self-modifying code — prioritized by measured
application requirements. Maintain an unsupported-feature list.

**Exit criteria.** Every newly supported feature has dedicated correctness tests.

---

## Phase 8 — AI optimization laboratory ⬜

**Objective.** Automated, evidence-gated optimization search.

**Work items.** Workflow letting an AI propose isolated optimizations, build
candidates, run tests, benchmark against a fixed baseline, and submit verified
changes for review. The baseline is immutable; failed tests cannot be bypassed.

**Exit criteria.** The pipeline rejects incorrect and slower candidates and
preserves successful ones.

---

## Phase 9 — Real application compatibility ⬜

**Objective.** Progressively demanding real programs.

**Work items.** Integration suite; library forwarding; graphics integration;
desktop applications; PRoot/Android constraints. Prefer upstream components
where safer than reinventing complex functionality.

**Kernel-dependent workloads (added by the 2026-10-19 scope correction).** For
programs that need real kernel semantics (services, containers, `systemd`),
evaluate the kernel-as-process tier against the reference device:

1. Build the AArch64 UML port and confirm it runs under Android 10's kernel
   with no root and no KVM (resolves R1/R2).
2. Measure its boot cost and syscall-boundary cost. No performance claim
   without numbers.
3. Implement it **only if** steps 1-2 succeed, behind the syscall-layer
   interface, and never as a replacement for the user-mode path.

**Exit criteria.** Published compatibility matrix with reproducible results,
stating plainly which workloads the user-mode path cannot run and whether the
kernel-as-process tier is available on the target.

---

## Phase 10 — Stabilization and release ⬜

**Objective.** A defensible experimental release.

**Work items.** Audit correctness, memory safety, resource consumption,
isolation, performance, portability, build reproducibility, documentation.

**Exit criteria.** A versioned experimental release whose documented exit
criteria are met. The project is never declared "complete" because a session
ended.

---

## Working agreement

Every milestone follows the loop in `docs/agent-handoff.md`: read the state,
pick the smallest meaningful task, state acceptance criteria, implement only
that scope, build, test, fix, update docs, commit, push, record the next step.
