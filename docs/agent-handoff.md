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

**Phases 0–4 are complete; Phase 5 (performance optimization) is under way, with
its baseline recorded and five optimizations accepted and measured.** The repository builds clean with `-Werror`, has a dependency-free
unit-test suite wired into CTest, and has a **verified cross-architecture
validation pipeline** that cross-compiles the test suite to AArch64 and runs it
under `qemu-user`. A host capability probe measures architecture/page size/cache
line/W^X rather than assuming them, and a benchmark harness reports repeated-run
statistics with guest-execution metrics marked unavailable. The project installs
with a single `curl | bash` command that builds, tests and installs a `hyperion`
command.

**Phase 2 added the first executable core.** There is a guest-state model
(`include/hyperion/guest_state.hpp`), an **x86-64 decoder for a documented
subset** that explicitly rejects everything else, and a **reference
interpreter** that is the correctness oracle. The interpreter is validated by a
**hardware differential test**: the same operations are executed natively on the
x86-64 host and compared against the interpreter's results
(`tests/differential/`).

**Phase 3 added the first code generation.** An AArch64 encoder
(`include/hyperion/aarch64_encoder.hpp`) emits the operations the translator
needs with every encoding checked against GNU as/objdump; an executable-memory
layer (`include/hyperion/exec_mem.hpp`) writes code, makes it read-execute
(W^X), and clears the instruction cache; and a **translator**
(`include/hyperion/translator.hpp`) lowers a decoded x86-64 instruction to a
self-contained `BlockResult block(GuestState*)` AArch64 block. It handles `mov`,
`add`, `sub`, `and`, `or`, `xor`, `cmp`, `inc`, `dec`, `nop`, `hlt` with
register/immediate operands, and computes all six x86 flags in generated code
(`DECISIONS.md` D-017).

**Phase 4 turns that into a basic dynamic binary translator.** `translate_block`
lowers a whole basic block — it decodes from a guest PC to a terminator and
lowers `jmp` (rel32 and rel8) and `jcc` (rel8), writing the authoritative next
`rip`. `TranslationCache` (`include/hyperion/translation_cache.hpp`) is the
dispatcher: it translates each guest PC once, caches the block, and runs blocks
until the program halts, an unsupported instruction is reached, or a
guest-instruction step limit is hit. It **never executes an instruction the
translator refused**. Whole-program differentials (a counted loop; taken and
not-taken branches) run under `qemu-user` and match the interpreter on final
state and step count. `hyperion run <file> --tier=0|1` runs a raw x86-64 blob
through either tier and the two must agree (`tests/cli/cli_smoke.sh`).

**What is still missing.** No block chaining, no register allocation (guest
registers stay in memory), and no cache invalidation (self-modifying code would
run stale translations — a documented boundary). The syscall layer exists but is
a **first slice only**: `write`/`exit`/`exit_group` are implemented, everything
else returns `-ENOSYS`. The guest stack (`push`/`pop`/`ret`) and memory operands
are still *refused explicitly* rather than miscompiled, because they need a guest
address space (Phase 6). The interpreter remains the only *complete* execution
path. Phase 5 continues with register allocation; `STATUS.md` §4 lists the steps.

**Two real defects were found and fixed in Phase 4**, both by testing: a block
whose *first* instruction was unsupported was emitted empty and made the
dispatcher loop forever (now reported as an error), and the code-fetch window
shrank only to 15 bytes so a 13-byte program could not be translated (now
shrinks to one byte).

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
scripts/dev.sh test         # expect: all tests passed
scripts/dev.sh cross-test   # expect: all tests passed incl. cross.aarch64_tests
./build/dev/tests/hyperion_differential_tests   # expect: 8 passed, 0 skipped (x86-64)
./build/dev/hyperion version
./build/dev/hyperion probe
python3 tools/gen_syscall_inventory.py --check   # expect: OK, inventory valid

# optional: the installer path (no root, installs into a temp prefix)
bash install.sh --src . --prefix /tmp/hp --yes
/tmp/hp/bin/hyperion doctor
bash install.sh --uninstall --prefix /tmp/hp
```

If `test` or `cross-test` does not pass, **stop and fix that first**. Do not
build new features on a broken base.

Expected test names: `unit.native`, `cli.smoke`, `differential.native_vs_interpreter`,
`syscall.inventory_valid`, `tier_bench.smoke`, `bench.smoke`, `bench.methodology`,
`bench.timing_not_eliminated`, `bench.rejects_single_sample`, and (cross build
only) `cross.aarch64_tests`. The suite has 9 tests natively and 10 in the cross
build.

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
9. `docs/syscalls/README.md` — the syscall inventory, its tiers and its
   evidence policy; then the seven generated files `U1.md`–`U4.md`, `E1.md`–`E3.md`.

---

## 5. Your next task

**Phases 0–4 are closed. Phase 5 is active (five optimizations accepted and
measured: parity fast path, dead-`rip` elimination, flag liveness, direct-mapped
dispatch, ALU immediate folding). Phase 6's syscall inventory is complete and
CI-validated, and the first syscall slice (`write`/`exit`/`exit_group`) is
implemented and tested in both tiers. The next concrete step is guest register
allocation (Phase 5), then the guest address space (Phase 6).** Guest state, the
decoder, the reference interpreter, the subset contract (`COMPATIBILITY.md` §2),
the hardware differential, the AArch64 encoder, executable memory,
single-instruction translation, whole-block translation, the translation-cache
dispatcher, and the syscall dispatch layer all exist and pass.

1. ~~Build an AArch64 encoder~~ **Done** (`a64::Encoder`). ~~Add executable-code
   management~~ **Done** (`ExecMem`). ~~Marshall `GuestState` and translate one
   instruction~~ **Done** (`translate_instruction`). ~~Translate a basic block~~ **Done**
   (`translate_block`). ~~Add a translation cache and dispatcher~~ **Done**
   (`TranslationCache`). Add encoder instructions as the translator needs them,
   each with a binutils-verified test.
2. ~~Close Phase 4 out~~ **Done.** Exit criterion met (nested loops, a branch
   inside a loop, and the earlier programs all match the interpreter), CI green,
   recorded in `STATUS.md` §2.
3. ~~Build the Tier-1-vs-Tier-0 timing harness and record a baseline~~ **Done**
   (`benchmarks/tier_bench.cpp`, `scripts/bench-tier-qemu.sh`, BENCHMARKS.md
   §4.2): Tier 1 = 38.4 ns/guest-ins vs Tier 0 = 491.9 ns/guest-ins (ratio
   0.078) on a fixed loop under `qemu-user`.
4. ~~Optimization 1: parity (PF) fast path~~ **Done & measured.** (BENCHMARKS.md
   §4.2.1; 904 → 644 bytes, Tier 1 wall time −17.5%.)
5. ~~Optimization 2: dead `rip` stores~~ **Done & measured.** (BENCHMARKS.md
   §4.2.2; 644 → 584 bytes, Tier 1 wall time ≈ −4%.)
5a. ~~Optimization 3: flag liveness~~ **Done & measured.** (BENCHMARKS.md
   §4.2.3; 584 → 352 bytes, −39.7%; size-guard 148 → 90 words. Guard:
   `translator.dead_flags_are_not_computed`.)
5b. ~~Optimization 4: direct-mapped dispatch~~ **Done & measured.** (BENCHMARKS.md
   §4.2.4; 99.9985% dispatch hit rate. Guard:
   `translation_cache.direct_map_collision_falls_back_to_map`.)
5c. ~~Optimization 5: ALU immediate folding~~ **Done & measured.** (BENCHMARKS.md
   §4.2.5; size-guard 90 → 88 words. Guard:
   `translator.alu_immediate_is_folded`.)
6. ~~Establish the seven-file syscall inventory~~ **Done.** `docs/syscalls/`
   holds `U1.md`–`U4.md`, `E1.md`–`E3.md`, generated from `inventory.tsv` and
   validated against the pinned Linux v6.12 table by
   `tools/gen_syscall_inventory.py` (ctest `syscall.inventory_valid`, CI job
   `inventory`). Tiers are evidence-backed via `scripts/trace-syscalls.sh`.
7. ~~Implement the `syscall` instruction and a minimal dispatch layer~~ **Done &
   tested.** `Opcode::kSyscall` (`0F 05`) is decoded; `SyscallHandler`
   (`include/hyperion/syscall.hpp`) receives the guest state and writes `rax`.
   Both tiers route to it (the translator exits with `BlockResult::kSyscall` and
   the dispatcher calls the handler). `write`(1), `exit`(60), `exit_group`(231)
   are implemented; every other number returns `-ENOSYS`, counted in
   `RunResult::unimplemented_syscalls`. 11 new tests, including a Tier 1
   differential under `qemu-user`.
8. **Next (Phase 5): guest register allocation (start small).** Keep the
   most-used guest registers in AArch64 registers *within* a block (load at
   entry, spill at exit), leaving the block signature and the authoritative-`rip`
   rule unchanged. Require the whole differential suite to pass, then re-run
   `scripts/bench-tier-qemu.sh` and record before/after in BENCHMARKS.md with an
   accept/reject verdict. Never change the baseline to make a candidate look
   good. Add a deterministic guard for whatever the change should move (e.g. a
   `GuestState` load/store count), since `qemu-user` timing is noisy (DECISIONS.md
   D-020).
9. **Then (Phase 6): build the guest address space.** Implement
   `mmap`/`munmap`/`mprotect`/`brk` over a guest-managed region with explicit
   bounds and permission checks, so the loader and dynamic programs can allocate
   memory. Keep the handler boundary: the guest never receives a raw host
   pointer, and every guest pointer is validated (as `write` already does). Then
   extend coverage in the U4 order (`docs/syscalls/U4.md`). Move the
   corresponding `status` values in `inventory.tsv` and regenerate.
10. **Keep the oracle.** Run each multi-instruction program through both the
    generated blocks and the interpreter's `run_program`, and require the final
    `GuestState` to match bit-for-bit. A mismatch must fail loudly; there is no
    tolerance for "close enough".
11. Everything runs on the real AArch64 path for free: the `cross.aarch64_tests`
    pipeline builds the tests for AArch64, runs them under `qemu-user`, and runs
    the CLI smoke test with `HYPERION_EMULATOR`, so generated code is exercised in
    CI without the target device.

**The interpreter stays.** It is the oracle. Do not weaken it to make the code
generator look correct.

Full Phase 5 and Phase 6 acceptance criteria are in `ROADMAP.md`; the concrete
first steps are in `STATUS.md` §4.

**Do not** write kernel-level code — the tier is unvalidated on the reference
device. **Do not** claim a syscall works until it has a test and its inventory
`status` reflects reality.

Optional Phase 1 leftovers, neither blocking Phase 5/6: unblocking the AArch64
UML build (a host glibc/kernel-header problem, see `docs/prior-art.md` §6), and a
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
