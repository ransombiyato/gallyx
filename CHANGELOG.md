# CHANGELOG

Meaningful changes by milestone. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions are pre-release.

---

## [Unreleased] — Phase 1

_Nothing yet._

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

### Known limitations
- No instruction decoding, interpretation or translation.
- Development host is x86-64; AArch64 validation is via `qemu-user` only.
- No reference-target (Snapdragon 855) measurements.
