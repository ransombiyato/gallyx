# Project HYPERION

**An experimental x86-64 → AArch64 user-mode dynamic binary translator.**

HYPERION explores how far a purpose-built, benchmark-driven dynamic binary
translator can go on ARM64 Linux devices, with a long-term goal of competitive
performance on selected real-world x86-64 workloads.

The reference target device is a **Qualcomm Snapdragon 855 running ARM64
Android 10, 8 GB RAM, no root and no KVM**. Development and testing also happen
on other Linux hosts, so target-device compatibility is *measured, not
assumed* — see [`docs/target-environment.md`](docs/target-environment.md).

> **Status: Phase 3 (first native code generation) complete at its stated
> scope.** The repository builds, its tests pass, and a documented subset of
> x86-64 instructions now executes through generated AArch64 code, verified
> against the reference interpreter under `qemu-user`. **There is no
> multi-instruction program execution, translation cache or syscall layer yet**,
> and `hyperion run` still refuses to run anything. See
> [`STATUS.md`](STATUS.md) for the exact current state and
> [`ROADMAP.md`](ROADMAP.md) for the staged plan.
>
> **Scope note (2026-10-19).** Kernel/system-mode emulation is no longer ruled
> out. A measured experiment showed a real Linux kernel running unprivileged
> with `systemd` as PID 1, and an AArch64 port of that mechanism exists. It is
> under investigation, not implemented — see
> [`docs/architecture-feasibility.md`](docs/architecture-feasibility.md).

---

## What is actually implemented today

| Capability | State |
| --- | --- |
| Build system (CMake, C++20) | Implemented, builds clean with `-Werror` |
| Unit tests + CTest integration | Implemented, passing |
| AArch64 cross-build + qemu-user validation pipeline | Implemented, passing |
| Host capability probe (arch, page size, cache line, W^X) | Implemented |
| Host benchmark harness (JSON, repeated-run statistics) | Implemented (host-only) |
| One-line installer + `hyperion` command | Implemented |
| x86-64 decoder (documented subset) | Implemented, differential-tested |
| Guest-state model + reference interpreter | Implemented, differential-tested |
| AArch64 encoder (checked, binutils-verified) | Implemented |
| W^X executable memory | Implemented |
| x86-64 → AArch64 instruction translation | **Partial** — register/immediate ALU ops only |
| Multi-instruction program translation / code cache | **Not started** (Phase 4) |
| Linux syscall layer | **Not started** (Phase 6) |

The translation row is deliberately narrow: the translator lowers a specific,
listed set of instructions (`mov`, `add`, `sub`, `and`, `or`, `xor`, `cmp`,
`inc`, `dec`, `nop`, `hlt` with register/immediate operands) and *refuses*
everything else with an explicit error. Control flow, the stack and memory
operands are not translated yet. See [`COMPATIBILITY.md`](COMPATIBILITY.md) §2.6.

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
src/core/               Core library (host probe, decoder, interpreter,
                        AArch64 encoder, executable memory, translator)
src/cli/                Command-line entry point
tests/harness/          Dependency-free C++ test harness
tests/unit/             Unit tests
tests/run_cross_tests.cmake  Cross-arch validation driver
tests/bench_rejects_bad_args.cmake  Negative test for the benchmark harness
benchmarks/             Benchmark harness
experiments/            Feasibility experiments (NOT part of the translator)
scripts/                Developer task runner, env bootstrap, `hyperion` CLI
install.sh              One-line installer (curl | bash)
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
| [`docs/architecture-feasibility.md`](docs/architecture-feasibility.md) | Scope analysis: user-mode vs system-mode vs hybrid, with measured evidence |

---

## Quick start

### One-line install

```bash
curl -fsSL https://raw.githubusercontent.com/ransombiyato/hyperion-dbt/main/install.sh | bash
```

This needs no root, writes nothing outside `~/.local` (override with
`--prefix`), builds and tests the project, and installs a `hyperion` command:

```bash
hyperion doctor    # check the host, toolchain and install
hyperion status    # what is implemented, and what is not
hyperion test      # run the test suite
hyperion bench     # run the benchmark harness
hyperion help      # every command
```

If you would rather read a script before running it — always reasonable — the
installer is a plain, commented shell script:

```bash
curl -fsSLO https://raw.githubusercontent.com/ransombiyato/hyperion-dbt/main/install.sh
less install.sh && bash install.sh
```

To remove it again: `bash install.sh --uninstall --prefix ~/.local`.

> **`hyperion run` does not work yet, on purpose.** Single instructions can be
> translated and executed, but there is no basic-block translation or code cache
> (Phase 4), so a whole guest program cannot run. It prints `NOT IMPLEMENTED`
> and exits 2. A command that silently did nothing would be worse than one that
> says so. `hyperion bench` measures the host machine, not any emulation.

### Build from source

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
test suite inside `qemu-aarch64`, and requires it to pass. Phase 3 already uses
this exact mechanism to validate generated AArch64 code against the x86-64
reference interpreter: `translator.differential_*` executes translated blocks
under `qemu-user` and compares every register and flag against the interpreter.

See [`TESTING.md`](TESTING.md) for the full guide.

---

## Scope and non-goals

**In scope**

- User-mode x86-64 Linux binary translation to AArch64.
- Correctness-first execution with a reference interpreter as oracle.
- Measured, reproducible performance work.
- **Investigation of kernel-level compatibility** (added 2026-10-19). A user-mode
  translator cannot provide `systemd` as PID 1, containers, or other
  kernel-dependent behaviour. How far those can be reached on an unprivileged
  device is analysed in
  [`docs/architecture-feasibility.md`](docs/architecture-feasibility.md), which
  records a measured experiment and a provisional design. **Nothing of this is
  implemented, and no kernel-level code will be written until it is validated on
  the reference device.**

**Explicit non-goals (for now)**

- Running Windows or macOS binaries.
- Requiring root or KVM on the target device.
- Bit-exact cycle-accurate CPU emulation.

**Withdrawn non-goal.** "System-mode or kernel emulation" was previously listed
here as out of scope. That was withdrawn on 2026-10-19 because it was decided
without measurement and it capped the project below its stated goal. See
`DECISIONS.md` D-011.

## Contributing

Read [`CONTRIBUTING.md`](CONTRIBUTING.md) and
[`docs/agent-handoff.md`](docs/agent-handoff.md). Work in small, verified
milestones. Correctness always outranks speed.

## Licence

Apache-2.0. See [`LICENSE`](LICENSE).
