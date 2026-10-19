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

## Phase 1 — Establish the baseline ✅ (item 6 blocked, see note)

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
   port and confirm the binary is produced.

**Exit criteria.**

- `THIRD_PARTY.md` and a baseline comparison document are committed. ✅
- `benchmarks/` produces repeatable, attributable results. ✅
- `BENCHMARKS.md` documents methodology and any measured baselines. ✅

**Evidence.** `docs/prior-art.md` (survey, licences, trade-offs, reuse matrix);
`DECISIONS.md` D-013 (reuse-vs-build boundary); `BENCHMARKS.md` §4 (measured
host calibration with spread and a scaling validation); `install.sh` +
`scripts/hyperion` (one-line install and a `hyperion` test-run command).

**Item 6 note.** The AArch64 UML port configures but does not compile. The
failure is not arch-specific — stock x86-64 UML fails identically on this host —
so it is a host glibc/kernel-header incompatibility, not a port defect. The port
is recorded as *plausible but unverified*; no claim is made that it works.
Transcript in `docs/prior-art.md` §6.

**Defect found and fixed in this phase:** BENCH-001, a benchmark harness that
reported a fabricated result because the optimiser eliminated the timed
workload. See `docs/known-issues.md`.

---

## Phase 2 — Minimal instruction execution 🟡

**Objective.** Correctly execute a small, explicitly documented instruction
subset.

**Work items.**

1. Define the subset in `COMPATIBILITY.md`. ✅ **Done** — §2 is now a precise
   specification of the implemented subset (and lists what is *not* implemented
   separately, so the plan is not mistaken for reality).
2. Implement the decoder for that subset. ✅ **Done** —
   `src/core/decoder.cpp`; unsupported input is rejected with a specific
   `DecodeError`.
3. Implement guest state and instruction semantics in the IR/interpreter.
   ✅ **Done** — `guest_state.*`, `interpreter.*`.
4. Build automated tests for registers, flags, arithmetic, memory, control flow.
   ✅ **Done** — 54 unit cases.
5. Differential-test the interpreter against an independent reference.
   ✅ **Done** — 8 cases against **real x86-64 hardware** (`tests/differential/`).

**Exit criteria.** The supported subset passes differential tests against an
independent reference. Unsupported instructions fail explicitly, never silently.
**Met for the implemented subset.** Not met for the *planned* items in
`COMPATIBILITY.md` §2.5 (`test`, `neg`, `not`, shifts, `imul`, `lea`, real
`call`/`ret`, `jcc rel32`, `syscall`, `div`), which are deferred.

**Remaining before Phase 3:** a convenience helper to run a multi-instruction
program image (with branches/loops) through the interpreter for testing. The
stepping semantics already exist.

---

## Phase 3 — First native code generation 🟡

**Objective.** Execute the Phase 2 subset through generated AArch64 code.

**Work items.**
1. AArch64 encoder/emitter — **Done.** `a64::Encoder`
   (`include/hyperion/aarch64_encoder.hpp`), every encoding reference-verified
   against GNU as/objdump (12 unit cases).
2. Executable memory management — **Done.** `ExecMem`
   (`include/hyperion/exec_mem.hpp`), W^X write-then-execute plus I-cache
   clear.
3. Executing generated code end to end — **Done for the encoder's subset.**
   `test_exec_mem.cpp` generates and *runs* `add` and a summation loop; both
   pass on AArch64 under `qemu-user` (and in CI's cross job), and skip
   honestly on x86-64.
4. Guest-state marshalling — **Done (increment 1).** A translated block has the
   signature `BlockResult block(GuestState*)` (AAPCS64, state pointer in `x8`);
   guest registers stay in the `GuestState` struct in memory and generated code
   loads/stores them. No guest-register allocation yet (Phase 5).
5. Translating decoded x86 instructions to AArch64 — **Done for the
   register/immediate subset.** `translate_instruction`
   (`include/hyperion/translator.hpp`) lowers `mov`, `add`, `sub`, `and`, `or`,
   `xor`, `cmp`, `inc`, `dec`, `nop` and `hlt` with 32/64-bit register or
   immediate operands. All six x86 flags (CF/PF/AF/ZF/SF/OF) are *computed in
   generated code* from AArch64 NZCV plus two ALU identities, not delegated to
   the interpreter. Control flow (`jmp`/`jcc`/`ret`) and the stack
   (`push`/`pop`) are refused explicitly (Phase 4); memory operands are refused
   explicitly (need an address space, Phase 6). A 847-case differential test
   executes the generated blocks under `qemu-user` and compares every register
   and all six flags against the interpreter.

**Exit criteria.** Verified test programs execute correctly through generated
AArch64 code, validated under `qemu-user` on x86-64 hosts and natively on
AArch64 hosts. **Met for the translated subset:** decoded x86-64 instructions
now execute through generated AArch64 code, and the differential suite is
exact against the reference interpreter. Multi-instruction *programs* (loops,
branches) remain Phase 4, so this is the completion of Phase 3's stated scope
rather than of program-level translation.

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
