# Agent handoff

**Read this first if you are a new AI agent continuing Project HYPERION.**

This document tells you exactly where the project is, how to verify it, and
what to do next. It is updated at the end of every work session. Do not rely on
conversation history — the repository is the memory.

---

## 1. What HYPERION is

An experimental **x86-64 → AArch64 user-mode dynamic binary translator**.
Reference target: Qualcomm Snapdragon 855, ARM64 Android 10, 8 GB RAM, no root,
no KVM. Development often happens on x86-64 Linux hosts.

---

## 2. Current state in one paragraph

**Phases 0, 1 and 2 are complete.** The repository
builds clean with `-Werror`, has a dependency-free unit-test suite wired into
CTest, and has a **verified cross-architecture validation pipeline** that
cross-compiles the test suite to AArch64 and runs it under `qemu-user`. A host
capability probe measures architecture/page size/cache line/W^X rather than
assuming them, and a benchmark harness reports repeated-run statistics with
guest-execution metrics marked unavailable. The project installs with a single
`curl | bash` command that builds, tests and installs a `hyperion` command.

**Phase 2 added the first executable core.** There is now a guest-state model
(`include/hyperion/guest_state.hpp`), an **x86-64 decoder for a documented
subset** that explicitly rejects everything else, and a **reference
interpreter** that is the correctness oracle for Phase 3. The interpreter is
validated by a **hardware differential test**: the same operations are executed
natively on the x86-64 host and compared against the interpreter's results
(`tests/differential/`).

**Phase 3 has started and proved its riskiest mechanism.** An AArch64 encoder
(`include/hyperion/aarch64_encoder.hpp`) emits a first set of operations with
every encoding checked against GNU as/objdump, and an executable-memory layer
(`include/hyperion/exec_mem.hpp`) writes code, makes it read-execute (W^X), and
clears the instruction cache. Generated AArch64 code is now actually executed:
`tests/unit/test_exec_mem.cpp` runs `add` and a summation loop under
`qemu-user` in CI. **What is still missing is the translator itself** — no
x86-64 instruction is lowered to AArch64 yet, there is no translation cache and
no syscall layer, and `GuestState` is not yet mapped onto AArch64 registers or
memory.

**Scope was corrected on 2026-10-19.** Kernel/system-mode emulation is no longer
an explicit non-goal (`DECISIONS.md` D-011). Kernel-level compatibility is in
scope for *investigation*; the user-mode translator remains the primary path.
The kernel-as-process option was de-risked by measurement and by desk research
(`docs/architecture-feasibility.md`). **No kernel-level code exists or should be
written yet.**

**Two real defects were found and fixed in Phase 2**, both by the differential
test — 64-bit ADD carry was never set, and `Jcc` short jumps were decoded as
rel32. A third issue was an *integrity* problem, not a code bug: the harness
could not report a test as "not executed", so the hardware-differential cases
would have claimed PASS on AArch64 while comparing nothing. `HYPERION_SKIP` was
added; on AArch64 the suite now reports `0 passed, 8 skipped`. When a test
cannot run, say so — never let it look like coverage.

**A real defect was found and fixed in Phase 1.** BENCH-001: the benchmark
harness reported a fabricated result because `-O2` eliminated the timed
workload. Read `docs/known-issues.md` §"Benchmarking defects" before touching
`benchmarks/`. The lesson generalises: a measurement that looks too good is
usually broken, and this project requires evidence, not plausible numbers.

> **The environment does not persist.** A new session starts with no toolchain.
> Run `scripts/bootstrap.sh` first, and do not treat a stale `build/` directory
> as evidence that tests pass — rerun them.

---

## 3. Verify the current state (do this first)

```bash
scripts/bootstrap.sh        # toolchain (sudo); skip if already installed
scripts/dev.sh bootstrap
scripts/dev.sh build
scripts/dev.sh test         # expect: 6/6 test entries passed
scripts/dev.sh cross-test   # expect: 3/3 tests passed incl. cross.aarch64_tests
./build/dev/tests/hyperion_differential_tests   # expect: 8 passed, 0 skipped (x86-64)
./build/dev/hyperion version
./build/dev/hyperion probe

# optional: the installer path (no root, installs into a temp prefix)
bash install.sh --src . --prefix /tmp/hp --yes
/tmp/hp/bin/hyperion doctor
bash install.sh --uninstall --prefix /tmp/hp
```

If `test` or `cross-test` does not pass, **stop and fix that first**. Do not
build new features on a broken base.

Expected test names: `unit.native`, `bench.smoke`, `bench.methodology`,
`bench.timing_not_eliminated`, `bench.rejects_single_sample`,
`cross.aarch64_tests`.

---

## 4. Read these, in order

1. `STATUS.md` — precise current state and the immediate next task.
2. `ROADMAP.md` — all phases, the scope correction, and acceptance criteria.
3. `ARCHITECTURE.md` — design and component boundaries.
4. `docs/architecture-feasibility.md` — why kernel-level scope was reopened,
   and the measured evidence.
5. `DECISIONS.md` — why things are the way they are (D-001 … D-013).
6. `CONTRIBUTING.md` — conventions and the milestone workflow.
7. `docs/known-issues.md` — current limitations, including BENCH-001.
8. `docs/prior-art.md` — the survey and the reuse-vs-build boundary.

---

## 5. Your next task

**Phase 2 is complete. Start Phase 3 — first native code generation.** Guest
state, the decoder, the reference interpreter, the subset contract
(`COMPATIBILITY.md` §2), the hardware differential and the program-image runner
all exist and pass.

1. ~~Build an AArch64 encoder~~ **Done** (`a64::Encoder`). ~~Add executable-code
   management~~ **Done** (`ExecMem`). The encoder currently covers
   add/sub/and/orr/eor (reg + imm), cmp/cmp-imm, mov/movz/movk/mov-imm64,
   b/b.cond/ret/nop. Add instructions to it as the translator needs them, each
   with a binutils-verified test.
2. **Next: define the generated-code calling convention and marshall
   `GuestState`.** Decide which AArch64 registers hold which guest registers
   (a fixed mapping is fine for the first translator), where the guest `rip` and
   flags live, and how a translated block is entered and left. Keep it simple and
   documented; optimisation comes later.
3. **Then: translate the Phase 2 subset one instruction at a time.** Lower a
   decoded `hyperion::Instruction` to `a64::Encoder` calls, run it, and compare
   the resulting `GuestState` against the interpreter oracle (extend
   `tests/differential/` or add a translator-vs-interpreter differential). A
   mismatch must fail loudly; there is no tolerance for "close enough".
4. Everything runs on the real AArch64 path for free: the `cross.aarch64_tests`
   pipeline already builds the tests for AArch64 and runs them under `qemu-user`
   in CI, so generated code is exercised without the target device.

**The interpreter stays.** It is the oracle. Do not weaken it to make the code
generator look correct.

Full Phase 3 acceptance criteria are in `ROADMAP.md`.

**Do not** write kernel-level code — the tier is unvalidated on the reference
device.

Optional Phase 1 leftovers, neither blocking Phase 3: unblocking the AArch64 UML
build (a host glibc/kernel-header problem, see `docs/prior-art.md` §6), and a
first CI performance-trend baseline.

---

## 6. Hard rules

1. **The repository is the memory.** Commit coherent changes; update `STATUS.md`
   and this file before you stop.
2. **Correctness outranks speed.** Never fabricate results, tests, or
   compatibility claims. Mark unavailable measurements unavailable.
3. **Work in small, verified milestones.** State acceptance criteria before
   coding; implement only that scope; build and test; fix failures; then commit.
4. **Component boundaries are inviolable.** The decoder must not know AArch64;
   the code generator must not know x86-64; the interpreter stays clear; the CLI
   holds no translation logic.
5. **No root, no KVM.** The core translator must run unprivileged on the target.
6. **W^X is assumed on AArch64.** Write code, then make it executable, then
   flush the instruction cache.
7. **Do not claim a phase complete because code exists.** Only evidence counts.
8. **Do not overwrite unrelated work.** Never touch existing Arch/FEXDroid
   installations or other projects.
9. **Never push a failing build or red CI.**
10. **Keep the syscall layer behind an interface.** It must be possible to
    direct syscalls to the host kernel or to a user-space kernel without
    rewriting the translator (`DECISIONS.md` D-011).
11. **Do not claim target-device support from host measurements.** Snapdragon
    855 / Android 10 has never been tested in this environment.

---

## 7. Environment specifics for this workspace

- **Development host is x86-64** (Debian 13). You cannot natively run AArch64
  binaries here — use the cross + qemu pipeline.
- **Nothing persists between sessions.** A fresh container has no `cmake`,
  `ninja`, `clang`, `qemu-aarch64`, or AArch64 cross compiler, even if a
  `build/` directory survives. Run `scripts/bootstrap.sh` before concluding
  anything is broken.
- **`qemu-user` is a correctness oracle only**, never a performance model.
- **Passwordless `sudo`** is available for package installation.
- The reference Snapdragon 855 device is **not** present; target-device claims
  must be measured on real hardware (`docs/target-environment.md`).
- **Branch policy (changed 2026-10-19):** develop directly on `main`. The
  maintainer asked for this. `phase-0-foundation` was fast-forwarded into
  `main`; do not open a pull request per milestone unless asked.

---

## 8. Where things live

| Need | Location |
| --- | --- |
| Build config | `CMakeLists.txt`, `CMakePresets.json`, `cmake/` |
| Core code | `src/core/`, headers in `include/hyperion/` |
| CLI | `src/cli/main.cpp` |
| Tests | `tests/harness/`, `tests/unit/` |
| Cross-arch driver | `tests/run_cross_tests.cmake` |
| Benchmarks | `benchmarks/` |
| Scripts | `scripts/dev.sh`, `scripts/bootstrap.sh` |
| CI | `.github/workflows/ci.yml` |
| Feasibility experiment | `experiments/uml/` |
| Architecture analysis | `docs/architecture-feasibility.md` |

Future component directories (`src/decoder/`, `src/ir/`, `src/interp/`,
`src/codegen/`, `src/runtime/`, `src/syscall/`) are created when their phase
begins — their absence is a deliberate signal of what does not exist yet.

`experiments/` is **not** part of the translator. It holds evidence-gathering
for architecture decisions and must never be linked into the product.

---

## 9. Session-end checklist

Before stopping for any reason:

- [ ] Working tree builds; relevant tests pass.
- [ ] Coherent changes committed (not left uncommitted).
- [ ] Pushed if credentials and permissions allow; **never claim a push
      succeeded unless it actually did**.
- [ ] `STATUS.md` updated: current state, last successful test, next task.
- [ ] `docs/agent-handoff.md` (this file) updated.
- [ ] `CHANGELOG.md` updated for the milestone.
- [ ] Failing tests and unresolved questions recorded.
- [ ] The single best next action written down explicitly.
