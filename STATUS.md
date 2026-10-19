# STATUS

**Last updated:** 2026-10-19
**Current phase:** Phase 0 complete → Phase 1 next
**Repository state:** buildable, tested, documented. **No guest execution.**

This is the first file a returning agent should read. It records exactly what
works, what was last verified, and what to do next.

---

## 1. Current state (one paragraph)

The repository is a working C++20/CMake foundation. It builds clean with
warnings-as-errors, has a dependency-free unit-test harness wired into CTest,
and — most importantly — has a **verified cross-architecture validation
pipeline**: the test suite is cross-compiled to AArch64 and executed under
`qemu-user` on this x86-64 host. A host capability probe measures (rather than
assumes) architecture, page size, cache-line size and W^X expectations, and a
benchmark harness emits JSON with guest-execution metrics explicitly marked
unavailable. There is **no decoder, interpreter, code generator or syscall
layer** yet.

---

## 2. Last successful verification

Run on 2026-10-19, host: AMD EPYC 9B14 (x86-64), Debian 13 (trixie), GCC 14.2.0,
CMake 3.31.6, Ninja 1.12.1, qemu-aarch64 10.0.13.

| Check | Command | Result |
| --- | --- | --- |
| Native build (`-Werror`) | `cmake --build build/dev` | ✅ 12/12 targets, no warnings |
| Native tests | `ctest --test-dir build/dev` | ✅ **2/2 passed** (`unit.native`, `bench.smoke`) |
| Cross build | `cmake --build build/cross-test` | ✅ AArch64 binaries produced |
| Cross tests under qemu | `ctest --test-dir build/cross-test` | ✅ **2/2 passed** (`unit.native`, `cross.aarch64_tests`) |
| AArch64 unit tests in qemu | `qemu-aarch64 build/cross-test/cross-aarch64/tests/hyperion_unit_tests` | ✅ 8 passed, 0 failed |
| AArch64 probe correctness | `qemu-aarch64 .../hyperion probe` | ✅ reports `arch: aarch64`, `assume W^X: yes`, cache line 32 |
| Benchmark harness | `hyperion_bench --iterations 100000 --json` | ✅ valid JSON, guest metrics `available:false` |

Unit tests currently cover: version string well-formedness, build-summary
content, host-probe architecture agreement with compile-time arch, plausible
CPU/page-size reporting, W^X assumption per architecture, and report formatting.

---

## 3. Failing tests / open questions

- **Failing tests:** none.
- **Open questions:** none blocking. See `docs/known-issues.md` for limitations.

---

## 4. Immediate next task

**Phase 1 — Establish the baseline.**

Smallest meaningful next task, in order:

1. **Prior-art survey.** Write `docs/prior-art.md` comparing FEX-Emu, Box64,
   QEMU TCG, and relevant decoder/compiler projects: licence, reuse potential,
   design trade-offs versus a custom implementation. No code reuse until this
   is committed (see `DECISIONS.md` D-002).
2. **Baseline benchmark methodology.** Extend `benchmarks/` with a documented,
   repeatable procedure and record measured host baselines in `BENCHMARKS.md`.
   Do not fabricate numbers; mark unavailable measurements as unavailable.
3. **Reuse-vs-build decision.** Record the chosen boundary in `DECISIONS.md`.

**Acceptance criteria for Phase 1** are in `ROADMAP.md`. Do not begin Phase 2
until they are met.

---

## 5. Exact commands to reproduce this state

```bash
scripts/bootstrap.sh            # one-time toolchain install (needs sudo)
scripts/dev.sh bootstrap        # configure
scripts/dev.sh build            # build
scripts/dev.sh test             # native tests
scripts/dev.sh cross-test       # AArch64 tests under qemu-user
scripts/dev.sh bench --iterations 100000 --json
```

---

## 6. Environment notes for the next agent

- The **development host is x86-64**, not AArch64. Do not assume you can run
  AArch64 binaries natively — use the cross + qemu pipeline
  (`scripts/dev.sh cross-test`). This is deliberate and reusable.
- The reference **target** device (Snapdragon 855, Android 10) is not present
  in this environment. Target-device claims must be measured on real hardware
  before being asserted; see `docs/target-environment.md`.
- Passwordless `sudo` is available in this environment for package installs.
- Branch policy: `main` is protected by convention — develop on a branch and
  open a pull request.
