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

**Phase 0 is complete.** The repository builds clean with `-Werror`, has a
dependency-free unit-test suite wired into CTest, and has a **verified
cross-architecture validation pipeline** that cross-compiles the test suite to
AArch64 and runs it under `qemu-user`. A host capability probe measures
architecture/page size/cache line/W^X rather than assuming them, and a benchmark
harness emits JSON with guest-execution metrics marked unavailable. **There is
no instruction decoding, interpretation, translation, or syscall layer.**
Phase 1 (baseline/prior-art) is the next phase.

---

## 3. Verify the current state (do this first)

```bash
scripts/bootstrap.sh        # toolchain (sudo); skip if already installed
scripts/dev.sh bootstrap
scripts/dev.sh build
scripts/dev.sh test         # expect: 2/2 tests passed
scripts/dev.sh cross-test   # expect: 2/2 tests passed incl. cross.aarch64_tests
./build/dev/hyperion version
./build/dev/hyperion probe
```

If `test` or `cross-test` does not pass, **stop and fix that first**. Do not
build new features on a broken base.

Expected test names: `unit.native`, `bench.smoke`, `cross.aarch64_tests`.

---

## 4. Read these, in order

1. `STATUS.md` — precise current state and the immediate next task.
2. `ROADMAP.md` — all phases and their acceptance criteria.
3. `ARCHITECTURE.md` — design and component boundaries.
4. `DECISIONS.md` — why things are the way they are (D-001 … D-010).
5. `CONTRIBUTING.md` — conventions and the milestone workflow.
6. `docs/known-issues.md` — current limitations.

---

## 5. Your next task

**Phase 1 — Establish the baseline.** Smallest meaningful first step:

> Write `docs/prior-art.md` comparing FEX-Emu, Box64, QEMU TCG, and relevant
> decoder/compiler projects: licence, reuse potential, design trade-offs versus
> a custom implementation.

Then extend `benchmarks/` with a documented methodology and record measured
host baselines in `BENCHMARKS.md`, and record the reuse-vs-build decision in
`DECISIONS.md`. Full Phase 1 acceptance criteria are in `ROADMAP.md`.

**Do not** start Phase 2 (instruction execution) until Phase 1 exit criteria
are met.

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

---

## 7. Environment specifics for this workspace

- **Development host is x86-64** (AMD EPYC, Debian 13, 4 CPUs, 15 GiB RAM).
  You cannot natively run AArch64 binaries here — use the cross + qemu pipeline.
- **`qemu-user` is a correctness oracle only**, never a performance model.
- **Passwordless `sudo`** is available for package installation.
- The reference Snapdragon 855 device is **not** present; target-device claims
  must be measured on real hardware (`docs/target-environment.md`).
- Branch policy: develop on `phase-<n>-<name>`; `main` is protected by
  convention; open a pull request.

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

Future component directories (`src/decoder/`, `src/ir/`, `src/interp/`,
`src/codegen/`, `src/runtime/`, `src/syscall/`) are created when their phase
begins — their absence is a deliberate signal of what does not exist yet.

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
