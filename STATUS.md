# STATUS

**Last updated:** 2026-10-19
**Current phase:** Phase 3 (first native code generation) — in progress
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
| 1. Guest-state representation (GPRs, RFLAGS) | **Done** — `include/hyperion/guest_state.hpp` |
| 2. x86-64 decoder for a documented subset | **Done** — `include/hyperion/decoder.hpp`, `src/core/decoder.cpp` |
| 3. Reference interpreter (correctness oracle) | **Done** — `src/core/interpreter.cpp` |
| 4. Unit tests: decoder, interpreter, guest state | **Done** — 42 new cases (see §2) |
| 5. Differential test vs. real x86-64 hardware | **Done** — `tests/differential/`, 8 cases |
| 6. Documented instruction subset in COMPATIBILITY.md | **Done** — §2 is the Phase 3 contract |
| 7. Branch/loop execution over a program image | **Done** — `run_program()` + loop tests |

**Instruction subset (Phase 2).** MOV, ADD, SUB, AND, OR, XOR, CMP, INC, DEC,
PUSH, POP, JMP (rel32), Jcc (rel8), NOP, HLT, and RET-as-stop, at 32- and
64-bit operand sizes, with REX/ModRM/SIB addressing. **The authoritative
specification is `COMPATIBILITY.md` §2**, which also lists the planned-but-
unbuilt instructions separately. Everything else is explicitly rejected with a
specific `DecodeError`; the decoder never guesses.

**`run_program()`.** A convenience layer over `step()` that runs a program
image until HLT/RET, an error, or a step limit. It adds no new semantics, but
it makes multi-instruction programs testable — `test_interpreter.cpp` runs a
real `dec`/`jnz` loop (sum 1..10 = 55) and cross-checks it against native
x86-64, and asserts that an infinite loop reports a step limit rather than
hanging.

**Two real bugs were found by the differential test and fixed** (this is the
whole point of having one):
1. 64-bit ADD carry was never set — `sum > mask` is impossible when the mask is
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
| 1. AArch64 encoder/emitter | **Done** — `a64::Encoder`, 18 reference-verified cases |
| 2. Executable memory (W^X + I-cache clear) | **Done** — `ExecMem`, 3 cases |
| 3. Run generated AArch64 code end to end | **Done** — `add` + summation loop pass under qemu |
| 4. Guest-state marshalling for generated code | **Done (increment 1)** — block signature `BlockResult block(GuestState*)` |
| 5. x86-64 → AArch64 instruction translation | **Done for the register/immediate subset** |

**What "done" means here, precisely.** `a64::Encoder` turns a small set of
AArch64 operations into 32-bit words, and every encoding is checked against the
output of GNU `as`/`objdump` rather than written from memory. `ExecMem`
allocates read-write, is filled, then becomes read-execute (W^X) with an
instruction-cache clear — see `DECISIONS.md` D-015, D-016.

`translate_instruction` (`include/hyperion/translator.hpp`) now lowers a decoded
x86-64 instruction to a self-contained AArch64 block. Supported today: `mov`,
`add`, `sub`, `and`, `or`, `xor`, `cmp`, `inc`, `dec`, `nop`, `hlt`, with 32/64-bit
register or immediate operands. Guest registers stay in the `GuestState` struct
in memory; the generated block loads and stores them (no register allocation
yet). Every one of the six x86 flags is *computed in generated code* from AArch64
NZCV plus two ALU identities, never delegated to the interpreter (D-017).

`tests/unit/test_translator.cpp` runs the generated blocks under `qemu-user` on
AArch64 and compares every register and all six flags against the interpreter —
a fixed instruction set plus an 847-case operand sweep over carry/overflow/parity
boundaries. On x86-64 the differential cases report **skip**, not a false pass.

**Why Phase 3 is closed at this scope.** The Phase 3 exit criterion — decoded
x86-64 instructions execute correctly through generated AArch64 code, validated
under `qemu-user` — is met for the translated subset. What remains is
*program-level* translation (loops, branches, block exits, a translation cache),
which is exactly Phase 4. Control flow (`jmp`/`jcc`/`ret`), the stack
(`push`/`pop`) and memory operands are refused explicitly with a `TranslateError`
rather than miscompiled.


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
statistics and guest-execution metrics explicitly marked unavailable.

Phase 2 added the first executable core: a **guest-state model**, an **x86-64
decoder for a documented subset** that rejects everything else explicitly, and a
**reference interpreter** that is the correctness oracle. Correctness is anchored
by a **differential test that compares the interpreter against real x86-64
hardware** (8 cases, thousands of comparisons across operand values and flag
inputs).

Phase 3 added the first code generation: a **checked AArch64 encoder**
(binutils-verified), **W^X executable memory**, and a **translator** that lowers
a subset of decoded x86-64 instructions to generated AArch64. Generated blocks
run under `qemu-user` in CI and are checked bit-for-bit against the interpreter,
including all six x86 flags. The translator currently handles single instructions
with register/immediate operands; there is still **no translation cache, no
block chaining, no multi-instruction program translation and no syscall layer**.
Control flow, the stack and memory operands are refused explicitly rather than
miscompiled, and the interpreter remains the only complete execution path.

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
| Native tests | `ctest --test-dir build/dev` | PASS — **6/6** entries (86 unit cases + 8 differential) |
| Differential vs. hardware | `./build/dev/tests/hyperion_differential_tests` | PASS — 8/8 compared against real x86-64 |
| Cross build | `cmake --build build/cross-test` | PASS — AArch64 binaries produced |
| Cross tests under qemu | `ctest --test-dir build/cross-test` | PASS — **3/3** |
| AArch64 differential | `qemu-aarch64 .../hyperion_differential_tests` | SKIP — 0 passed, **8 skipped** (no x86-64 oracle) |
| AArch64 unit tests in qemu | `qemu-aarch64 .../hyperion_unit_tests` | PASS — 85 passed, 0 failed |
| Generated-code execution (AArch64) | `exec_mem.runs_generated_*` under qemu | PASS - `add` and a summation loop run correctly |
| Translator vs. interpreter (AArch64) | `translator.differential_*` under qemu | PASS — 847 operand cases, all registers + all 6 flags match |
| ASan/UBSan (clang) | `ctest --test-dir build/sanitize` | PASS — 6/6 |
| Clang build + tests | `ctest --test-dir build/clang` | PASS — 6/6 |
| Formatting | `clang-format --dry-run --Werror` | PASS — clean |
| Shell scripts | `shellcheck install.sh scripts/*` | PASS — clean |
| Installer (local) | `install.sh --src . --prefix DIR` | PASS — builds, tests, installs, uninstalls |
| Installer (curl) | `curl .../install.sh \| bash` | PASS — end-to-end from GitHub |
| UML feasibility (smoke) | `experiments/uml/run.sh smoke` | PASS — kernel boots unprivileged; all 6 namespace types OK |
| UML feasibility (systemd) | `experiments/uml/run.sh systemd` | PASS — systemd 257 as PID 1 → `multi-user.target` |

The native suite is 6 CTest entries: `unit.native` (one entry running **86**
unit test cases across 9 files), `differential.native_vs_interpreter` (**8**
cases compared against real x86-64 hardware), `bench.smoke`,
`bench.methodology`, `bench.timing_not_eliminated` and
`bench.rejects_single_sample`.

### Remote verification (GitHub Actions)

CI run `39272332010` on `main` (commit `d676695`, Phase 3 encoder + exec
memory): **conclusion `success`** — all five jobs passed: `AArch64 cross tests
(qemu-user)` (42s), `ASan/UBSan` (31s), `clang-format` (6s), `build & test
(clang)` (27s), `build & test (gcc)` (26s). The AArch64 job is the one that
executes generated code (`exec_mem.runs_generated_*`) under `qemu-user`.

CI run `39269782167` on `main` (commit `049fb99`, Phase 2 complete):
**conclusion `success`** — all five jobs passed.

CI run `39268700143` on `main` (commit `6c70b87`, Phase 2): **conclusion
`success`** — all five jobs passed: `clang-format` (7s), `AArch64 cross tests
(qemu-user)` (41s), `ASan/UBSan` (30s), `build & test (clang)` (32s), `build &
test (gcc)` (40s).

CI run `39263916932` on `main` (commit `cadbc17`): **conclusion `success`** —
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
    the reference device either. See `docs/prior-art.md` §6 and
    `docs/architecture-feasibility.md` §9.
  - Whether Android 10's seccomp/SELinux policy permits the `ptrace`/mmap
    behaviour the kernel-as-process tier needs is unmeasured (R2).
  - No measurement exists on the reference device (Snapdragon 855, Android 10).
    Everything recorded so far is from the x86-64 development host.
- See `docs/known-issues.md` for limitations.

---

## 4. Immediate next task

**Phase 3 is complete at its stated scope. The next task is Phase 4: turn
single-instruction translation into a basic dynamic binary translator.**

What exists now: `a64::Encoder`, `ExecMem`, and `translate_instruction`, which
lowers `mov`/`add`/`sub`/`and`/`or`/`xor`/`cmp`/`inc`/`dec`/`nop`/`hlt` with
register/immediate operands to a `BlockResult block(GuestState*)` AArch64 block,
verified bit-for-bit against the interpreter under `qemu-user` (see §0b).

The single best next action is to **translate a basic block, not a single
instruction**:

1. **Basic-block formation.** Starting at a guest PC, decode instructions until
   a terminator (`jmp`/`jcc`/`ret`/`hlt`) or an unsupported instruction. Record
   the guest addresses of each instruction and of the block's exit(s).
2. **Guest `rip` at block exit.** Make the generated block write the correct
   next guest PC: the branch target for a taken `jmp`/`jcc`, the fall-through
   address otherwise, and leave `rip` alone on `hlt`. This is where a block's
   return value should also say *why* it stopped (fall-through vs. branch vs.
   halt), so the dispatcher can find the next block.
3. **Control-flow lowering.** `jcc` needs the x86 condition evaluated from the
   modelled flags — reuse the same NZCV-derived logic already in the translator
   rather than a new path. `ret` needs a guest stack, which needs a guest address
   space; keep `ret`/`push`/`pop` refused until that exists.
4. **A translation cache and dispatcher.** Keyed by guest PC, holding translated
   blocks, with an explicit "unsupported" result that stops the run rather than
   executing something wrong. Cache invalidation can be trivial (never invalidate)
   for the first version, but the boundary must be documented.
5. Extend `tests/unit/test_translator.cpp` with multi-instruction programs
   (a counted loop, a conditional branch taken and not taken) and compare the
   final `GuestState` against `run_program` from the interpreter.

Add encoder instructions only as the translator needs them, each with a
binutils-verified test (see `TESTING.md` §7).

**The interpreter stays.** It is the oracle. Do not weaken it to make the
translator look correct. Phase 4 exit criteria are in `ROADMAP.md`.

Optional Phase 1 leftovers remain if wanted (unblocking the UML build, a CI
performance-trend baseline), but neither blocks Phase 4.

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
