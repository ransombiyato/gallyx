# STATUS

**Last updated:** 2026-10-19
**Current phase:** Phase 0 complete -> Phase 1 next (scope corrected, see below)
**Repository state:** buildable, tested, documented. **No guest execution.**

This is the first file a returning agent should read. It records exactly what
works, what was last verified, and what to do next.

---

## 1. Current state (one paragraph)

The repository is a working C++20/CMake foundation. It builds clean with
warnings-as-errors, has a dependency-free unit-test harness wired into CTest,
and has a **verified cross-architecture validation pipeline**: the test suite is
cross-compiled to AArch64 and executed under `qemu-user`. A host capability
probe measures (rather than assumes) architecture, page size, cache-line size
and W^X expectations, and a benchmark harness emits JSON with guest-execution
metrics explicitly marked unavailable. There is **no decoder, interpreter, code
generator or syscall layer** yet.

Two things changed in this session, and both matter more than the code:

1. **Scope was corrected.** The Phase 0 plan treated kernel/system-mode
   emulation as an explicit non-goal. That capped the project's compatibility
   ceiling before anything was measured, and it has been withdrawn
   (`DECISIONS.md` D-011). Kernel-level compatibility is now in scope for
   *investigation*, not implementation.
2. **The kernel-as-process option was de-risked by measurement.** A real Linux
   kernel was booted as an unprivileged process with no root and no KVM, and
   `systemd` ran as PID 1 to `multi-user.target`. An AArch64 port of that
   mechanism was located and verified to exist. See
   `docs/architecture-feasibility.md`.

The primary architecture is unchanged: the **user-mode translator** is still the
main path, and Phases 2-5 still build it.

---

## 2. Last successful verification

Re-verified from a clean tree on 2026-10-19 in a fresh container, host: x86-64,
Debian 13 (trixie), GCC 14.2.0, Clang 19.1.7, CMake 3.31.6, Ninja 1.12.1,
qemu-aarch64 10.0.13, aarch64-linux-gnu-g++ 14.2.0.

> **Environment note (important for the next agent).** This environment does
> **not** persist between sessions. A new container starts with *no* toolchain:
> no `cmake`, `ninja`, `clang`, `qemu-aarch64`, or AArch64 cross compiler. The
> `build/` directory from a previous session may still exist and look valid
> (including genuine AArch64 ELF binaries), but the tools to rebuild it will be
> missing. Run `scripts/bootstrap.sh` first. Do not report a test as passing
> because an artifact from a previous session is present - rerun it.

| Check | Command | Result |
| --- | --- | --- |
| Native build (`-Werror`) | `cmake --build build/dev` | PASS - 12/12 targets, no warnings |
| Native tests | `ctest --test-dir build/dev` | PASS - **2/2** |
| Cross build | `cmake --build build/cross-test` | PASS - AArch64 binaries produced |
| Cross tests under qemu | `ctest --test-dir build/cross-test` | PASS - **2/2** |
| AArch64 unit tests in qemu | `qemu-aarch64 .../hyperion_unit_tests` | PASS - 8 passed, 0 failed |
| AArch64 probe | `qemu-aarch64 .../hyperion probe` | PASS - `arch: aarch64`, `assume W^X: yes`, cache line 32 |
| ASan/UBSan (clang) | `ctest --test-dir build/sanitize` | PASS - 2/2 |
| Clang build + tests | `ctest --test-dir build/clang` | PASS - 2/2 |
| Formatting | `clang-format --dry-run --Werror` | PASS - clean |
| UML feasibility (smoke) | `experiments/uml/run.sh smoke` | PASS - kernel boots unprivileged; all 6 namespace types OK |
| UML feasibility (systemd) | `experiments/uml/run.sh systemd` | PASS - systemd 257 as PID 1 -> `multi-user.target`, `graphical.target` |

### Remote verification (GitHub Actions)

Phase 0 CI run `39244780989` (commit `460629a`): **conclusion `success`** -
`build & test (gcc)`, `build & test (clang)`, `AArch64 cross tests (qemu-user)`,
`ASan/UBSan`, `clang-format`.

Pull request: <https://github.com/ransombiyato/hyperion-dbt/pull/1>

Unit tests cover: version string well-formedness, build-summary content,
host-probe architecture agreement with compile-time arch, plausible CPU/page-size
reporting, W^X assumption per architecture, and report formatting.

---

## 3. Failing tests / open questions

- **Failing tests:** none.
- **Open questions:**
  - The AArch64 UML port has **not** been built or run by this project, and not
    on the reference device (Snapdragon 855 / Android 10). See
    `docs/architecture-feasibility.md` section 9.
  - Whether Android 10's seccomp/SELinux policy permits the `ptrace`/mmap
    behaviour the kernel-as-process tier needs is unmeasured (R2).
- See `docs/known-issues.md` for limitations.

---

## 4. Immediate next task

**Phase 1 - Establish the baseline.** The scope correction adds one item but
does not reorder the plan.

Smallest meaningful next task, in order:

1. **Prior-art survey.** Write `docs/prior-art.md` comparing FEX-Emu, Box64,
   QEMU TCG, and relevant decoder/compiler projects: licence, reuse potential,
   design trade-offs versus a custom implementation. No code reuse until this
   is committed (`DECISIONS.md` D-002).
2. **Baseline benchmark methodology.** Extend `benchmarks/` with a documented,
   repeatable procedure and record measured host baselines in `BENCHMARKS.md`.
   Do not fabricate numbers; mark unavailable measurements as unavailable.
3. **Reuse-vs-build decision.** Record the chosen boundary in `DECISIONS.md`.
4. **Cross-build the AArch64 UML port** (new, optional, small). Building it
   would confirm R1 far more strongly than reading a README does. This is
   investigation, not a commitment to the tier.

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

experiments/uml/run.sh smoke    # UML feasibility: kernel boots unprivileged
experiments/uml/run.sh systemd  # UML feasibility: systemd as PID 1 (slow)
experiments/uml/run.sh clean    # remove generated images/logs
```

---

## 6. Environment notes for the next agent

- The **development host is x86-64**, not AArch64. Use the cross + qemu
  pipeline (`scripts/dev.sh cross-test`) to execute AArch64 code.
- **Nothing persists between sessions** - see the note in section 2. Reinstall
  the toolchain before concluding that something is broken.
- The reference **target** device (Snapdragon 855, Android 10) is not present in
  this environment. Target-device claims must be measured on real hardware
  before being asserted; see `docs/target-environment.md`.
- Passwordless `sudo` is available for package installs.
- **Branch policy (changed):** development happens directly on `main` at the
  maintainer's request. `phase-0-foundation` has been fast-forwarded into
  `main` and is now redundant. Do not open a PR per milestone unless asked.
- `experiments/uml/` is evidence-gathering for an architecture decision. It
  contains **no emulation code** and is not part of the translator.
