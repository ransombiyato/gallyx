# CHANGELOG

Meaningful changes by milestone. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions are pre-release.

---

## [Unreleased] — Phase 1

### Added — one-line installer and `hyperion` command
- `install.sh`: `curl -fsSL .../install.sh | bash` fetches, builds and tests the
  project, then installs a `hyperion` command into `<prefix>/bin` (default
  `~/.local`). No root, nothing written outside the prefix, and it aborts
  rather than install a build that fails its own tests.
- `scripts/hyperion`: a front end with `status`, `doctor`, `version`, `build`,
  `test`, `bench`, `cross-test`, `clean`. `run` and `info` exist but exit 2 with
  `NOT IMPLEMENTED` — there is no translator yet, and an honest refusal is
  better than a command that silently does nothing.

### Added — baseline benchmark methodology
- `include/hyperion/bench_stats.hpp`, `src/core/bench_stats.cpp`: min, p25,
  median, p75, max, mean, population standard deviation and `iqr_fraction` (the
  noise indicator), computed in the core library so the Phase 8 optimisation lab
  reports results the same way.
- The harness now takes `--repeats` (default 9, minimum 2), discards one warm-up
  sample and reports the full spread; JSON schema `hyperion.bench.host.v1` →
  `v2`.
- Measured host calibration recorded in `BENCHMARKS.md` §4.1: median
  **2.0568 ns/op**, 1.2 % spread, checksum stable, with a scaling validation at
  1e6/1e7/1e8 iterations. These are harness figures, not a translator claim.
- 8 unit tests for the statistics; `bench.methodology` and
  `bench.timing_not_eliminated` CTest entries; `bench.rejects_single_sample`
  negative test.

### Fixed — BENCH-001, a fabricated benchmark result
- The Phase 0 harness reported ~0.00003 ns/op (~30 ps per iteration, far below
  one clock cycle) because `-O2` hoisted the loop-invariant calibration call out
  of the repeat loop. The checksum stayed correct, so the failure was silent.
- Found by checking that total time scales with `--iterations`; it did not.
- Fixed with an `asm` optimisation barrier plus a plausibility check: a result
  faster than one clock cycle per iteration is rejected, and the harness exits
  non-zero, rather than publishing it. Regression test added.

### Added — prior-art survey and reuse decision
- `docs/prior-art.md`: FEX-Emu, Box64, QEMU TCG, Zydis, Capstone, LLVM,
  Unicorn, Dynarmic, with licences verified against upstream licence files.
- `DECISIONS.md` D-013: the reuse-vs-build boundary.
- `docs/prior-art.md` §6: the attempted AArch64 UML cross-build. It configures
  but does not compile, and the same failure occurs on stock x86-64 UML, so it
  is a host toolchain problem rather than a port defect. The port is recorded as
  *plausible but unverified*.

### Changed — scope correction (2026-10-19)
- **Withdrawn** the Phase 0 assumption that kernel/system-mode emulation is an
  explicit non-goal. It capped the project's compatibility ceiling below its
  actual goal and was made without measurement. See `DECISIONS.md` D-011.
- Added `docs/architecture-feasibility.md`: an evaluation of user-mode
  translation, system-mode emulation, and hybrid kernel-as-a-process designs,
  with the Android host-kernel constraints that bound all of them.
- Added `DECISIONS.md` D-012 recording the kernel-as-process tier as
  **provisional**, pending validation on the reference device.
- Added a constraint on Phase 6: the syscall layer must be abstracted behind an
  interface so syscalls can be directed to the host kernel or a user-space
  kernel. Nothing about that second path is implemented.

### Added — feasibility evidence
- `experiments/uml/` — a reproducible experiment that boots a **real Linux
  kernel as an unprivileged user-space process** (no root, no KVM) and drives
  it to a `systemd` multi-user boot.
- Measured result: UML kernel `6.12.101` booted from an isolated ext4 image
  (host root verified *not* visible), `systemd 257.13-1~deb13u1` ran as PID 1
  and reached `multi-user.target` and `graphical.target`; `dbus`,
  `systemd-logind` and `getty@tty1` started; all six namespace types worked
  inside the guest.
- Desk research confirming an **AArch64** UML port exists
  (`zalexdev/linux-um-arm64`, `ARCH=um SUBARCH=arm64`), verified to contain real
  architecture code and Android/bionic support. **Not** built or run by this
  project, and **not** tested on the reference device.

### Changed — documentation
- `ARCHITECTURE.md` gained a scope section stating that it describes the
  user-mode translator and pointing to the feasibility analysis.
- `ROADMAP.md` gained a scope-correction section; Phase 9 gained a
  kernel-dependent-workload work item; Phase 1 gained an optional cross-build
  of the AArch64 UML port.
- `STATUS.md` rewritten to record the environment-persistence finding and the
  corrected scope.
- `.gitignore` covers UML experiment artifacts.

### Fixed — process
- Recorded that this environment **does not persist between sessions** and that
  stale `build/` artifacts from a previous session must not be mistaken for
  passing tests. All Phase 0 results were re-verified from a clean tree.
- `experiments/uml/run.sh` strips ANSI escapes before matching systemd target
  lines (systemd embeds colour codes inside target names, which silently broke
  the original pattern).

---

## [0.1.0] — 2026-10-19 — Phase 0: Project foundation

The first milestone: a buildable, testable, documented repository. **No guest
execution.** No instruction is decoded, interpreted or translated.

### Added — build system
- CMake ≥ 3.20 project (`CMakeLists.txt`), C++20, no compiler extensions.
- Compiler warning set (GCC/Clang) with optional `-Werror`.
- `CMakePresets.json` with `debug`, `release`, `sanitize`, `cross-aarch64`.
- AArch64 cross-compilation toolchain file
  (`cmake/toolchains/aarch64-linux-gnu.cmake`).

### Added — core library
- `hyperion::version_string()` and `hyperion::build_summary()` (`src/core/version.cpp`).
- Host capability probe (`src/core/host_probe.cpp`): architecture, OS, kernel,
  logical CPUs, page size, cache-line size, transparent hugepages, W^X
  expectation. Measures the host rather than assuming it (D-008).
- CLI (`src/cli/main.cpp`) with `version`, `probe`, `help`.

### Added — tests
- Dependency-free C++ test harness (`tests/harness/`) with registration,
  `CHECK`/`CHECK_EQ`, filtering and CTest integration (D-006).
- Unit tests for version and host probe (8 tests).
- Cross-architecture validation pipeline: AArch64 cross-build executed under
  `qemu-user`, enforced as the `cross.aarch64_tests` CTest test (D-007).

### Added — benchmarks
- Host benchmark harness (`benchmarks/host_bench.cpp`) with deterministic
  calibration and JSON output; guest-execution metrics explicitly marked
  unavailable (D-009).

### Added — tooling and environment
- `scripts/dev.sh` developer task runner; `scripts/bootstrap.sh` environment
  bootstrap.
- `.clang-format`, `.clang-tidy`, `.editorconfig`, `.gitignore`.
- GitHub Actions CI (`.github/workflows/ci.yml`).

### Added — documentation
- `README.md`, `ARCHITECTURE.md`, `ROADMAP.md`, `STATUS.md`, `DECISIONS.md`
  (D-001 … D-010), `COMPATIBILITY.md`, `BENCHMARKS.md`, `TESTING.md`,
  `CONTRIBUTING.md`, `SECURITY.md`, `THIRD_PARTY.md`, `CHANGELOG.md`.
- `docs/agent-handoff.md`, `docs/known-issues.md`, `docs/target-environment.md`,
  `docs/glossary.md`.

### Verified
- Native build: 12/12 targets, clean with `-Werror`.
- `unit.native` and `bench.smoke`: pass.
- `cross.aarch64_tests`: pass; AArch64 unit tests 8/8 under `qemu-aarch64`.
- AArch64 probe correctly reports `arch: aarch64` and `assume W^X: yes`.
- Pushed to `origin` as branch `phase-0-foundation`; GitHub Actions run
  `39244780989` (commit `460629a`) concluded `success` for all jobs: gcc build
  and tests, clang build and tests, AArch64 cross tests under `qemu-user`,
  ASan/UBSan, and clang-format. Pull request #1 opened against `main`.

### Known limitations
- No instruction decoding, interpretation or translation.
- Development host is x86-64; AArch64 validation is via `qemu-user` only.
- No reference-target (Snapdragon 855) measurements.
