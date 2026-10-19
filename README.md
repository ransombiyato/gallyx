# Project HYPERION

**An experimental x86-64 → AArch64 user-mode dynamic binary translator.**

HYPERION explores how far a purpose-built, benchmark-driven dynamic binary
translator can go on ARM64 Linux devices, with a long-term goal of competitive
performance on selected real-world x86-64 workloads.

The reference target device is a **Qualcomm Snapdragon 855 running ARM64
Android 10, 8 GB RAM, no root and no KVM**. Development and testing also happen
on other Linux hosts, so target-device compatibility is *measured, not
assumed* — see [`docs/target-environment.md`](docs/target-environment.md).

> **Status: Phase 0 (foundation).** The repository builds, its tests pass, and
> the documentation is complete. **There is no instruction execution yet.**
> No guest program can be run. See [`STATUS.md`](STATUS.md) for the exact
> current state and [`ROADMAP.md`](ROADMAP.md) for the staged plan.

---

## What is actually implemented today

| Capability | State |
| --- | --- |
| Build system (CMake, C++20) | Implemented, builds clean with `-Werror` |
| Unit tests + CTest integration | Implemented, passing |
| AArch64 cross-build + qemu-user validation pipeline | Implemented, passing |
| Host capability probe (arch, page size, cache line, W^X) | Implemented |
| Host benchmark harness (JSON output) | Implemented (host-only) |
| x86-64 decoder | **Not started** |
| Guest execution / interpreter | **Not started** |
| AArch64 code generator | **Not started** |
| Linux syscall layer | **Not started** |

This table is the single source of truth for "what works". It is updated at
every milestone. If a capability is not listed as implemented, assume it does
not exist.

---

## Repository map

```
CMakeLists.txt          Top-level build definition
CMakePresets.json       Standard configure/build/test presets
cmake/toolchains/       AArch64 cross-compilation toolchain file
include/hyperion/       Public headers
src/core/               Core library (version, host probe)
src/cli/                Command-line entry point
tests/harness/          Dependency-free C++ test harness
tests/unit/             Unit tests
tests/run_cross_tests.cmake  Cross-arch validation driver
benchmarks/             Benchmark harness
scripts/                Developer task runner and environment bootstrap
docs/                   Agent handoff, known issues, target environment
.github/workflows/      CI
```

Documentation index:

| File | Purpose |
| --- | --- |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | System design and component boundaries |
| [`ROADMAP.md`](ROADMAP.md) | All phases, milestones and completion status |
| [`STATUS.md`](STATUS.md) | Current state, last successful test, next task |
| [`DECISIONS.md`](DECISIONS.md) | Architectural decisions and reasoning |
| [`COMPATIBILITY.md`](COMPATIBILITY.md) | Implemented vs missing instruction/ABI features |
| [`BENCHMARKS.md`](BENCHMARKS.md) | Benchmark methodology and measured results |
| [`TESTING.md`](TESTING.md) | Build, test, fuzzing and differential-testing guide |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Coding conventions and workflow |
| [`SECURITY.md`](SECURITY.md) | Threat model and safe execution boundaries |
| [`CHANGELOG.md`](CHANGELOG.md) | Meaningful changes by milestone |
| [`THIRD_PARTY.md`](THIRD_PARTY.md) | Dependency and licence inventory |
| [`docs/agent-handoff.md`](docs/agent-handoff.md) | Instructions for a new agent |
| [`docs/known-issues.md`](docs/known-issues.md) | Bugs, limitations and blocked work |
| [`docs/target-environment.md`](docs/target-environment.md) | Reference device and host matrix |
| [`docs/glossary.md`](docs/glossary.md) | Terminology used in this project |

---

## Quick start

Requires a C++20 compiler and CMake ≥ 3.20.

```bash
# Install the full toolchain (Debian/Ubuntu; needs sudo once).
scripts/bootstrap.sh

# Configure, build, test.
scripts/dev.sh bootstrap
scripts/dev.sh build
scripts/dev.sh test

# Inspect the build and the host it is running on.
./build/dev/hyperion version
./build/dev/hyperion probe
```

### Cross-architecture validation

Because the reference target is AArch64 but development often happens on
x86-64, HYPERION validates AArch64 artifacts by cross-compiling and executing
them under `qemu-user`:

```bash
scripts/dev.sh cross-test
```

This configures a second build tree with `aarch64-linux-gnu-g++`, runs the same
test suite inside `qemu-aarch64`, and requires it to pass. Phase 3 onwards
reuses this exact mechanism to validate generated AArch64 code against the
x86-64 reference interpreter.

See [`TESTING.md`](TESTING.md) for the full guide.

---

## Scope and non-goals

**In scope**

- User-mode x86-64 Linux binary translation to AArch64.
- Correctness-first execution with a reference interpreter as oracle.
- Measured, reproducible performance work.

**Explicit non-goals (for now)**

- System-mode or kernel emulation.
- Running Windows or macOS binaries.
- Requiring root or KVM on the target device.
- Bit-exact cycle-accurate CPU emulation.

## Contributing

Read [`CONTRIBUTING.md`](CONTRIBUTING.md) and
[`docs/agent-handoff.md`](docs/agent-handoff.md). Work in small, verified
milestones. Correctness always outranks speed.

## Licence

Apache-2.0. See [`LICENSE`](LICENSE).
