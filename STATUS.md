# STATUS

**Last updated:** 2026-10-19
**Current phase:** Phase 1 (Establish the baseline) — in progress, most exit criteria met
**Repository state:** buildable, tested, documented. **No guest execution.**

This is the first file a returning agent should read. It records exactly what
works, what was last verified, and what to do next.

---

## 0. Phase 1 progress

| Work item | State |
| --- | --- |
| 1. Prior-art survey (FEX, Box64, QEMU TCG, decoders) | **Done** — `docs/prior-art.md` |
| 2. Licence / trade-off comparison | **Done** — `docs/prior-art.md` §2, §3, `THIRD_PARTY.md` |
| 3. Reuse-vs-build boundary recorded | **Done** — `DECISIONS.md` D-013 |
| 4. Reproducible benchmark harness | **Done** — `benchmarks/`, `include/hyperion/bench_stats.hpp` |
| 5. Baseline numbers recorded, unavailable marked unavailable | **Done** — `BENCHMARKS.md` §4 |
| 6. Cross-build the AArch64 UML port (optional) | **Attempted, blocked** — see below |
| — One-line installer + `hyperion` command | **Done** (extra, requested) — `install.sh` |

**Item 6 result.** The AArch64 UML port (`zalexdev/linux-um-arm64`, Linux
7.2.0-rc4) configures with `ARCH=um SUBARCH=arm64` but fails to compile. The
failure is **not** arch-specific: stock **x86-64** UML fails at the identical
header line on this host, so it is a host glibc/kernel-header incompatibility.
The port is therefore **plausible but unverified**, and no claim is made that it
works. Full transcript in `docs/prior-art.md` §6.

**Exit criteria status.** `THIRD_PARTY.md` and the baseline comparison are
committed; `benchmarks/` produces repeatable, attributable results; `BENCHMARKS.md`
documents methodology and measured baselines. Phase 1 exit criteria are met
except that item 6 (optional) could not be completed for an environmental
reason that is now documented.

---

## 1. Current state (one paragraph)

The repository is a working C++20/CMake foundation. It builds clean with
warnings-as-errors, has a dependency-free unit-test harness wired into CTest,
and has a **verified cross-architecture validation pipeline**: the test suite is
cross-compiled to AArch64 and executed under `qemu-user`. A host capability
probe measures (rather than assumes) architecture, page size, cache-line size
and W^X expectations, and a benchmark harness emits JSON with repeated-run
statistics and guest-execution metrics explicitly marked unavailable. There is
**no decoder, interpreter, code generator or syscall layer** yet.

The project also installs with a single command:

```bash
curl -fsSL https://raw.githubusercontent.com/ransombiyato/hyperion-dbt/main/install.sh | bash
```

which builds, tests and installs a `hyperion` command. `hyperion run` exists but
deliberately refuses to do anything: there is no translator yet, and a command
that silently did nothing would be worse than an honest refusal.

Two things changed in an earlier session, and both matter more than the code:

1. **Scope was corrected.** The Phase 0 plan treated kernel/system-mode
   emulation as an explicit non-goal. That capped the project's compatibility
   ceiling before anything was measured, and it has been withdrawn
   (`DECISIONS.md` D-011). Kernel-level compatibility is now in scope for
   *investigation*, not implementation.
2. **The kernel-as-process option was de-risked by measurement.** A real Linux
   kernel was booted as an unprivileged process with no root and no KVM, and
   `systemd` ran as PID 1 to `multi-user.target`. See
   `docs/architecture-feasibility.md`.

The primary architecture is unchanged: the **user-mode translator** is still the
main path, and Phases 2-5 still build it.

---

## 2. Last successful verification

Re-verified from a clean tree on 2026-10-19, host: x86-64, Debian 13 (trixie),
GCC 14.2.0, Clang 19.1.7, CMake 3.31.6, Ninja 1.12.1, qemu-aarch64 10.0.13,
aarch64-linux-gnu-g++ 14.2.0.

> **Environment note (important for the next agent).** This environment does
> **not** persist between sessions. A new container starts with *no* toolchain:
> no `cmake`, `ninja`, `clang`, `qemu-aarch64`, or AArch64 cross compiler. The
> `build/` directory from a previous session may still exist and look valid
> (including genuine AArch64 ELF binaries), but the tools to rebuild it will be
> missing. Run `scripts/bootstrap.sh` first. Do not report a test as passing
> because an artifact from a previous session is present - rerun it.

| Check | Command | Result |
| --- | --- | --- |
| Native build (`-Werror`) | `cmake --build build/dev` | PASS — no warnings |
| Native tests | `ctest --test-dir build/dev` | PASS — **5/5** |
| Cross build | `cmake --build build/cross-test` | PASS — AArch64 binaries produced |
| Cross tests under qemu | `ctest --test-dir build/cross-test` | PASS — **2/2** |
| AArch64 unit tests in qemu | `qemu-aarch64 .../hyperion_unit_tests` | PASS — 0 failed |
| AArch64 probe | `qemu-aarch64 .../hyperion probe` | PASS — `arch: aarch64`, `assume W^X: yes`, cache line 32 |
| ASan/UBSan (clang) | `ctest --test-dir build/sanitize` | PASS — 5/5 |
| Clang build + tests | `ctest --test-dir build/clang` | PASS — 5/5 |
| Formatting | `clang-format --dry-run --Werror` | PASS — clean |
| Shell scripts | `shellcheck install.sh scripts/*` | PASS — clean |
| Installer (local) | `install.sh --src . --prefix DIR` | PASS — builds, tests, installs, uninstalls |
| Installer (curl) | `curl .../install.sh \| bash` | PASS — end-to-end from GitHub |
| UML feasibility (smoke) | `experiments/uml/run.sh smoke` | PASS — kernel boots unprivileged; all 6 namespace types OK |
| UML feasibility (systemd) | `experiments/uml/run.sh systemd` | PASS — systemd 257 as PID 1 → `multi-user.target` |

The native suite is 5 CTest entries: `unit.native` (one entry running **16**
unit test cases), `bench.smoke`, `bench.methodology`,
`bench.timing_not_eliminated` and `bench.rejects_single_sample`.

### Remote verification (GitHub Actions)

CI run `39250218783` on `main` (commit `3f15e48`): **conclusion `success`** —
all five jobs passed: `build & test (gcc)`, `build & test (clang)`,
`AArch64 cross tests (qemu-user)`, `ASan/UBSan`, `clang-format`.

Earlier Phase 0 run `39244780989` (commit `460629a`) also passed.

One non-failing annotation appears on every job: `actions/checkout@v4` targets
Node.js 20, which the runner now forces onto Node.js 24. It is a deprecation
notice, not a failure; tracked as ENV-003 in `docs/known-issues.md`.

Unit tests cover: version string well-formedness, build-summary content,
host-probe architecture agreement with compile-time arch, plausible CPU/page-size
reporting, W^X assumption per architecture, report formatting, and benchmark
statistics (median, quartiles, population standard deviation, and a
stable-vs-noisy discrimination case).

---

## 3. Failing tests / open questions

- **Failing tests:** none.
- **Known defects:** BENCH-001 (a fabricated benchmark result) was found and
  fixed this session; see `docs/known-issues.md`.
- **Open questions:**
  - The AArch64 UML port has **not** been built or run. Its build is blocked by
    a host toolchain problem that also blocks stock x86-64 UML. Not measured on
    the reference device either. See `docs/prior-art.md` §6 and
    `docs/architecture-feasibility.md` §9.
  - Whether Android 10's seccomp/SELinux policy permits the `ptrace`/mmap
    behaviour the kernel-as-process tier needs is unmeasured (R2).
  - No measurement exists on the reference device (Snapdragon 855, Android 10).
    Everything recorded so far is from the x86-64 development host.
- See `docs/known-issues.md` for limitations.

---

## 4. Immediate next task

**Phase 1 is essentially complete. The next task is Phase 2 — minimal
instruction execution.**

Smallest meaningful next task:

1. Choose and document a small, explicit x86-64 instruction subset (registers,
   flags, a handful of integer ALU ops, a few memory forms, a few control-flow
   forms). Record it in `COMPATIBILITY.md`.
2. Implement guest state (the register file and flags word) and decoding for
   that subset.
3. Implement a reference interpreter for the subset — this becomes the
   correctness oracle for Phase 3's code generator.
4. Differential-test it against an independent reference (for example, execute
   the same instruction encodings natively on the x86-64 dev host and compare
   architectural state).

**Acceptance criteria for Phase 2** are in `ROADMAP.md`. Do not begin Phase 3
until they are met.

Two optional Phase 1 follow-ups remain if wanted: unblocking the UML build, and
a first CI performance-trend baseline. Neither blocks Phase 2.

---

## 5. Exact commands to reproduce this state

```bash
scripts/bootstrap.sh            # one-time toolchain install (needs sudo)
scripts/dev.sh bootstrap        # configure
scripts/dev.sh build            # build
scripts/dev.sh test             # native tests
scripts/dev.sh cross-test       # AArch64 tests under qemu-user
scripts/dev.sh bench --iterations 1000000 --repeats 9 --json

# installer (no root, installs into ~/.local by default)
curl -fsSL https://raw.githubusercontent.com/ransombiyato/hyperion-dbt/main/install.sh | bash
hyperion doctor && hyperion test && hyperion bench

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
