# STATUS

**Last updated:** 2026-10-19
**Current phase:** Phase 3 (first native code generation) ÔÇö in progress
**Repository state:** buildable, tested, documented. **The reference interpreter
executes a documented x86-64 subset; an AArch64 encoder and W^X executable-memory
layer are in place and run generated AArch64 code end to end, but no x86-64
instruction is translated to AArch64 yet.**

This is the first file a returning agent should read. It records exactly what
works, what was last verified, and what to do next.

---

## 0a. Phase 2 progress (minimal instruction execution)

| Work item | State |
| --- | --- |
| 1. Guest-state representation (GPRs, RFLAGS) | **Done** ÔÇö `include/hyperion/guest_state.hpp` |
| 2. x86-64 decoder for a documented subset | **Done** ÔÇö `include/hyperion/decoder.hpp`, `src/core/decoder.cpp` |
| 3. Reference interpreter (correctness oracle) | **Done** ÔÇö `src/core/interpreter.cpp` |
| 4. Unit tests: decoder, interpreter, guest state | **Done** ÔÇö 42 new cases (see ┬º2) |
| 5. Differential test vs. real x86-64 hardware | **Done** ÔÇö `tests/differential/`, 8 cases |
| 6. Documented instruction subset in COMPATIBILITY.md | **Done** ÔÇö ┬º2 is the Phase 3 contract |
| 7. Branch/loop execution over a program image | **Done** ÔÇö `run_program()` + loop tests |

**Instruction subset (Phase 2).** MOV, ADD, SUB, AND, OR, XOR, CMP, INC, DEC,
PUSH, POP, JMP (rel32), Jcc (rel8), NOP, HLT, and RET-as-stop, at 32- and
64-bit operand sizes, with REX/ModRM/SIB addressing. **The authoritative
specification is `COMPATIBILITY.md` ┬º2**, which also lists the planned-but-
unbuilt instructions separately. Everything else is explicitly rejected with a
specific `DecodeError`; the decoder never guesses.

**`run_program()`.** A convenience layer over `step()` that runs a program
image until HLT/RET, an error, or a step limit. It adds no new semantics, but
it makes multi-instruction programs testable ÔÇö `test_interpreter.cpp` runs a
real `dec`/`jnz` loop (sum 1..10 = 55) and cross-checks it against native
x86-64, and asserts that an infinite loop reports a step limit rather than
hanging.

**Two real bugs were found by the differential test and fixed** (this is the
whole point of having one):
1. 64-bit ADD carry was never set ÔÇö `sum > mask` is impossible when the mask is
   `~0`. Corrected to `(sum < ua) || (sum > mask)`.
2. `Jcc` short jumps (`0x70..0x7F`) were decoded as rel32 instead of rel8,
   producing wild jump targets.

**Integrity fix.** The test harness previously had no way for a test to report
"not executed", so the hardware-differential cases would have reported *PASS*
on AArch64 while comparing nothing. A `HYPERION_SKIP` mechanism was added; on
AArch64 the differential suite now honestly reports **0 passed, 8 skipped**.

## 0b. Phase 3 progress (first native code generation)

| Work item | State |
| --- | --- |
| 1. AArch64 encoder/emitter | **Done** — `a64::Encoder`, 12 reference-verified cases |
| 2. Executable memory (W^X + I-cache clear) | **Done** — `ExecMem`, 3 cases |
| 3. Run generated AArch64 code end to end | **Done** — `add` + summation loop pass under qemu |
| 4. Guest-state marshalling for generated code | **Not started** |
| 5. x86-64 → AArch64 instruction translation | **Not started** |

**What "done" means here, precisely.** `a64::Encoder` turns a small set of
AArch64 operations into 32-bit words, and every encoding is checked against the
output of GNU `as`/`objdump` rather than written from memory. `ExecMem`
allocates read-write, is filled, then becomes read-execute (W^X) with an
instruction-cache clear — see `DECISIONS.md` D-015, D-016. `test_exec_mem.cpp`
then generates `add x0,x0,x1; ret` and a `1..n` summation loop, executes them,
and checks the results; those two cases pass on AArch64 under `qemu-user` and
report **skip** on x86-64 rather than a false pass.

**Why Phase 3 is still open.** The generated code above is hand-written AArch64,
not a translation of x86-64. No x86-64 instruction is lowered to AArch64 yet,
and `GuestState` is not yet mapped onto AArch64 registers/memory. So the
mechanism is proven but this is not yet a translator.


---

## 0. Phase 1 progress

| Work item | State |
| --- | --- |
| 1. Prior-art survey (FEX, Box64, QEMU TCG, decoders) | **Done** ÔÇö `docs/prior-art.md` |
| 2. Licence / trade-off comparison | **Done** ÔÇö `docs/prior-art.md` ┬º2, ┬º3, `THIRD_PARTY.md` |
| 3. Reuse-vs-build boundary recorded | **Done** ÔÇö `DECISIONS.md` D-013 |
| 4. Reproducible benchmark harness | **Done** ÔÇö `benchmarks/`, `include/hyperion/bench_stats.hpp` |
| 5. Baseline numbers recorded, unavailable marked unavailable | **Done** ÔÇö `BENCHMARKS.md` ┬º4 |
| 6. Cross-build the AArch64 UML port (optional) | **Attempted, blocked** ÔÇö see below |
| ÔÇö One-line installer + `hyperion` command | **Done** (extra, requested) ÔÇö `install.sh` |

**Item 6 result.** The AArch64 UML port (`zalexdev/linux-um-arm64`, Linux
7.2.0-rc4) configures with `ARCH=um SUBARCH=arm64` but fails to compile. The
failure is **not** arch-specific: stock **x86-64** UML fails at the identical
header line on this host, so it is a host glibc/kernel-header incompatibility.
The port is therefore **plausible but unverified**, and no claim is made that it
works. Full transcript in `docs/prior-art.md` ┬º6.

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
statistics and guest-execution metrics explicitly marked unavailable.

Phase 2 added the first executable core: a **guest-state model**, an **x86-64
decoder for a documented subset** that rejects everything else explicitly, and a
**reference interpreter** that is now the correctness oracle for the future code
generator. Correctness is anchored by a **differential test that compares the
interpreter against real x86-64 hardware** (8 cases, thousands of comparisons
across operand values and flag inputs). There is still **no AArch64 code
generator, translation cache or syscall layer**; the interpreter is the only
way to execute guest code, and only one instruction at a time.

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
| Native build (`-Werror`) | `cmake --build build/dev` | PASS ÔÇö no warnings |
| Native tests | `ctest --test-dir build/dev` | PASS ÔÇö **6/6** entries (75 unit cases + 8 differential) |
| Differential vs. hardware | `./build/dev/tests/hyperion_differential_tests` | PASS ÔÇö 8/8 compared against real x86-64 |
| Cross build | `cmake --build build/cross-test` | PASS ÔÇö AArch64 binaries produced |
| Cross tests under qemu | `ctest --test-dir build/cross-test` | PASS ÔÇö **3/3** |
| AArch64 differential | `qemu-aarch64 .../hyperion_differential_tests` | SKIP ÔÇö 0 passed, **8 skipped** (no x86-64 oracle) |
| AArch64 unit tests in qemu | `qemu-aarch64 .../hyperion_unit_tests` | PASS ÔÇö 0 failed |
| Generated-code execution (AArch64) | `exec_mem.runs_generated_*` under qemu | PASS - `add` and a summation loop run correctly |
| ASan/UBSan (clang) | `ctest --test-dir build/sanitize` | PASS ÔÇö 6/6 |
| Clang build + tests | `ctest --test-dir build/clang` | PASS ÔÇö 6/6 |
| Formatting | `clang-format --dry-run --Werror` | PASS ÔÇö clean |
| Shell scripts | `shellcheck install.sh scripts/*` | PASS ÔÇö clean |
| Installer (local) | `install.sh --src . --prefix DIR` | PASS ÔÇö builds, tests, installs, uninstalls |
| Installer (curl) | `curl .../install.sh \| bash` | PASS ÔÇö end-to-end from GitHub |
| UML feasibility (smoke) | `experiments/uml/run.sh smoke` | PASS ÔÇö kernel boots unprivileged; all 6 namespace types OK |
| UML feasibility (systemd) | `experiments/uml/run.sh systemd` | PASS ÔÇö systemd 257 as PID 1 ÔåÆ `multi-user.target` |

The native suite is 6 CTest entries: `unit.native` (one entry running **58**
unit test cases across 8 files), `differential.native_vs_interpreter` (**8**
cases compared against real x86-64 hardware), `bench.smoke`,
`bench.methodology`, `bench.timing_not_eliminated` and
`bench.rejects_single_sample`.

### Remote verification (GitHub Actions)

CI run `39268700143` on `main` (commit `6c70b87`, Phase 2): **conclusion
`success`** ÔÇö all five jobs passed: `clang-format` (7s), `AArch64 cross tests
(qemu-user)` (41s), `ASan/UBSan` (30s), `build & test (clang)` (32s), `build &
test (gcc)` (40s).

CI run `39263916932` on `main` (commit `cadbc17`): **conclusion `success`** ÔÇö
all five jobs passed: `clang-format` (8s), `build & test (gcc)` (22s),
`ASan/UBSan` (33s), `AArch64 cross tests (qemu-user)` (31s), `build & test
(clang)` (24s).

Earlier green runs on `main`: `39263212528` (installer, `708c831`),
`39261295636` (benchmark methodology, `fc5fd42`), `39250218783` (`3f15e48`),
`39244780989` (`460629a`).

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
    the reference device either. See `docs/prior-art.md` ┬º6 and
    `docs/architecture-feasibility.md` ┬º9.
  - Whether Android 10's seccomp/SELinux policy permits the `ptrace`/mmap
    behaviour the kernel-as-process tier needs is unmeasured (R2).
  - No measurement exists on the reference device (Snapdragon 855, Android 10).
    Everything recorded so far is from the x86-64 development host.
- See `docs/known-issues.md` for limitations.

---

## 4. Immediate next task

**Phase 2 is complete and verified.** Guest state, the decoder, the reference
interpreter, the documented subset contract (`COMPATIBILITY.md` ┬º2), the
hardware differential and the program-image runner all exist and pass. The
Phase 2 exit criterion ÔÇö "the supported subset passes differential tests
against an independent reference" ÔÇö is met for the implemented subset.

The next task is **Phase 3 ÔÇö first native code generation**:

1. Build an AArch64 instruction encoder/emitter for the Phase 2 subset. Start
   with a tiny set (`mov`, `add`, `sub`, `and`, `orr`, `eor`, `cmp`, `b.cond`)
   and grow it as the generator needs it.
2. Add executable-code management (an `mmap`'d W^X region, `__builtin___clear_cache`)
   and a calling convention for entering/leaving generated code with the
   `GuestState` in memory.
3. Generate code one instruction at a time first (a "trivial" translator), then
   validate every result against the interpreter oracle using the existing
   differential harness. **The interpreter stays** ÔÇö it is the oracle.
4. Run the generated code on the real AArch64 path: the `cross.aarch64_tests`
   pipeline already executes AArch64 binaries under `qemu-user`, so generated
   code can be tested in CI without the target device.

Do not delete or weaken the interpreter to make the code generator look
correct. Phase 3 exit criteria are in `ROADMAP.md`.

Optional Phase 1 leftovers remain if wanted (unblocking the UML build, a CI
performance-trend baseline), but neither blocks Phase 3.

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
