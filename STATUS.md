# STATUS

**Last updated:** 2026-10-19
**Current phase:** Phase 4 (basic dynamic binary translator) — in progress
**Repository state:** buildable, tested, documented. **Two execution tiers now
exist: the reference interpreter (Tier 0) and a translation cache that runs
whole programs through generated AArch64 code (Tier 1). Multi-instruction
programs with `jmp`/`jcc` control flow run correctly through Tier 1 and are
checked bit-for-bit against the interpreter. The guest subset remains small;
see COMPATIBILITY.md.**

This is the first file a returning agent should read. It records exactly what
works, what was last verified, and what to do next.

---

## 0c. Phase 4 progress (basic dynamic binary translator)

| Work item | State |
| --- | --- |
| 1. Basic-block formation (decode to a terminator) | **Done** — `translate_block` |
| 2. Block exit reporting (`BlockExit`, `BlockResult`) | **Done** — `include/hyperion/translator.hpp` |
| 3. Control-flow lowering (`jmp` rel32/rel8, `jcc` rel8) | **Done** — writes authoritative `rip` |
| 4. Translation cache + dispatcher | **Done** — `include/hyperion/translation_cache.hpp`, `src/core/translation_cache.cpp` |
| 5. `hyperion run --tier=0|1` CLI | **Done** — `src/cli/main.cpp` |
| 6. Whole-program differential tests | **Done** — loops and branches vs `run_program` |
| 7. Block chaining / hot-path optimization | **Not started** — Phase 5 |
| 8. Cache invalidation (self-modifying code) | **Not started** — the cache never invalidates; documented boundary |
| 9. Guest stack (`push`/`pop`/`ret`) | **Refused** — needs a guest address space (Phase 6) |

**What "done" means here, precisely.** `translate_block` decodes from a guest
PC until it reaches a terminator (`jmp`/`jcc`/`hlt`), the end of the supplied
buffer, or an instruction it cannot translate. It returns the generated code,
the exit kind, a fall-through/target hint, and the number of guest instructions
the block covers. The authoritative next PC is always the `rip` field that the
generated block writes into `GuestState`; the cache is only a lookup
accelerator.

`TranslationCache::run` is the dispatcher: it looks up the block for
`state.rip`, translates it on first sight, executes it, and repeats until the
program halts, an unsupported instruction is reached, or a guest-instruction
step limit is hit. It never executes an instruction the translator refused.

**Three real bugs were found by testing this increment and fixed:**

1. **Infinite loop on an unsupported first instruction.** A block whose *first*
   instruction is unsupported was emitted as a zero-instruction fall-through
   block, so the dispatcher re-entered the same PC forever. `translate_block`
   now reports `kUnsupportedOpcode` when the block would be empty. (This is the
   "never silently execute incorrect code" rule, and the reason the block cutter
   distinguishes "cut before an unsupported instruction" from "the first
   instruction is unsupported".)
2. **Short program images failed to translate.** The dispatcher's code-fetch
   window shrank only down to 15 bytes, so a 13-byte blob (the CLI smoke
   program) could not be read at all. The window now shrinks to a single byte.
3. **The dispatcher could exceed the step limit.** It checked the limit *before*
   running a block and then added the whole block's instruction count, so a run
   limited to 1,000,000 instructions could retire 1,000,001 (found by the
   nested-loop differential, which compared against the interpreter). A block is
   indivisible, so the dispatcher now refuses to *start* a block that does not
   fit in the remaining budget; a step-limited run may stop a few instructions
   short of the limit but never exceeds it.

**Decoder addition.** `0xEB` (`jmp rel8`) was added — a very common real-world
encoding and the natural companion to the `0x70..0x7F` short `jcc`. It has a
decoder test (`decoder.jmp_short_rel8`).

**Why the guest subset is still small.** Phase 4's exit criterion is that a
repeatable suite of small guest programs runs correctly under the translator,
not that the instruction set is large. The subset is unchanged from Phase 3
plus control flow; `push`/`pop`/`ret` and memory operands stay refused because
they need a guest address space (Phase 6).

**Guest program suite.** Five whole programs run through the dispatcher and are
compared against the interpreter: a single counted loop, taken/not-taken
branches across all flag inputs, **nested loops** (two backward branches), a
**branch inside a loop** whose taken and not-taken exits reconverge and whose
direction alternates each iteration, and an infinite loop (reported as a step
limit, not a hang). Two of them also assert an independently computed final
value, so a case cannot pass by the interpreter and the translator agreeing on
the same mistake.

---

## 0b. Phase 3 progress (first native code generation)

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
including all six x86 flags.

Phase 4 turns that into a **basic dynamic binary translator**: `translate_block`
lowers a whole basic block (decode to a terminator, lower `jmp`/`jcc`, write the
authoritative next `rip`), and a **translation cache / dispatcher** runs complete
guest programs block by block, keyed by guest PC. The `hyperion run --tier=0|1`
command runs a raw x86-64 blob through either tier, and the two tiers are
required to agree. Whole-program differentials (a counted loop, taken and
not-taken conditional branches) pass under `qemu-user`. There is still **no
block chaining, no register allocation, no cache invalidation and no syscall
layer**. The guest stack and memory operands are refused explicitly rather than
miscompiled, and the interpreter remains the only *complete* execution path.

The project also installs with a single command:

```bash
curl -fsSL https://raw.githubusercontent.com/ransombiyato/hyperion-dbt/main/install.sh | bash
```

which builds, tests and installs a `hyperion` command. `hyperion run <file>`
executes a raw x86-64 code blob through the reference interpreter (`--tier=0`)
or the AArch64 translator (`--tier=1`). On a host that cannot execute generated
AArch64 code (an x86-64 development host), tier 1 refuses and says so rather
than pretending.

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
| Native tests | `ctest --test-dir build/dev` | PASS — **7/7** entries (98 unit cases + 8 differential) |
| Differential vs. hardware | `./build/dev/tests/hyperion_differential_tests` | PASS — 8/8 compared against real x86-64 |
| CLI smoke (both tiers) | `ctest --test-dir build/dev -R cli.smoke` | PASS — tiers agree, `steps=17` |
| Cross build | `cmake --build build/cross-aarch64` | PASS — AArch64 binaries produced |
| Cross tests under qemu | `qemu-aarch64 .../hyperion_unit_tests` | PASS — **96 passed, 0 failed, 2 skipped** |
| AArch64 differential | `qemu-aarch64 .../hyperion_differential_tests` | SKIP — 0 passed, **8 skipped** (no x86-64 oracle) |
| Translator blocks vs. interpreter (AArch64) | `translator.block_*` under qemu | PASS — block cuts at terminators/unsupported/buffer end |
| Whole-program differentials (AArch64) | `translator.differential_program_*` under qemu | PASS — counted loop, nested loops, branch-inside-loop all match `run_program` |
| Dispatcher limits (AArch64) | `translator.dispatcher_*` under qemu | PASS — stops at unsupported; step limit is a hard bound, does not hang |
| Translation cache (AArch64) | `translation_cache.caches_*` under qemu | PASS — caches blocks, agrees with interpreter |
| CLI tier 1 executes generated code | `HYPERION_EMULATOR=qemu-aarch64 ... cli_smoke.sh` | PASS — **3 basic blocks translated**, `steps=17` |
| ASan/UBSan (clang) | `ctest --test-dir build/san` | PASS — 7/7 |
| Clang build + tests | `ctest --test-dir build/clang` | PASS — 7/7 |
| Formatting | `clang-format --dry-run --Werror` | PASS — clean |
| Shell scripts | `shellcheck install.sh scripts/*` | PASS — clean |
| Installer (local) | `install.sh --src . --prefix DIR` | PASS — builds, tests, installs, uninstalls |
| Installer (curl) | `curl .../install.sh \| bash` | PASS — end-to-end from GitHub |
| UML feasibility (smoke) | `experiments/uml/run.sh smoke` | PASS — kernel boots unprivileged; all 6 namespace types OK |
| UML feasibility (systemd) | `experiments/uml/run.sh systemd` | PASS — systemd 257 as PID 1 → `multi-user.target` |

The native suite is 7 CTest entries: `unit.native` (one entry running **98**
unit test cases across 10 files), `differential.native_vs_interpreter` (**8**
cases compared against real x86-64 hardware), `cli.smoke`, `bench.smoke`,
`bench.methodology`, `bench.timing_not_eliminated` and
`bench.rejects_single_sample`.

### Remote verification (GitHub Actions)

CI run `39294174801` on `main` (commit `2148492`, Phase 4 translator):
**conclusion `success`** — all five jobs passed: `ASan/UBSan` (3m4s),
`build & test (gcc)` (27s), `clang-format` (6s), `build & test (clang)` (34s),
`AArch64 cross tests (qemu-user)` (54s). The AArch64 job executes the translated
blocks under `qemu-user`, runs the whole-program differentials, and runs the CLI
smoke test with `HYPERION_EMULATOR`, so tier 1 is proven to translate and run
generated code in CI.

CI run `39281904991` on `main` (commit `f1feee1`, Phase 3 translator):
**conclusion `success`** — all five jobs passed: `AArch64 cross tests (qemu-user)`
(1m18s), `build & test (gcc)` (49s), `clang-format` (44s), `ASan/UBSan` (39s),
`build & test (clang)` (24s). The AArch64 job is the one that executes the
translated blocks and runs `translator.differential_*` under `qemu-user`.

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
  fixed in an earlier session; see `docs/known-issues.md`.
- **Known limitations of the Phase 4 dispatcher:**
  - The translation cache **never invalidates**. Self-modifying guest code will
    run stale translations. This is a deliberate, documented boundary for the
    first version; invalidation is Phase 7 work.
  - Blocks are **not chained**: every block exit returns to the dispatcher, which
    does a hash lookup. This is correct but slow; chaining is Phase 5.
  - Guest code is fetched through a 4 KiB window and the block is cut at the
    window edge. Longer blocks are correct (they just split), not miscompiled.
- **Open questions:**
  - The AArch64 UML port has **not** been built or run. Its build is blocked by
    a host toolchain problem that also blocks stock x86-64 UML. Not measured on
    the reference device either. See `docs/prior-art.md` §6 and
    `docs/architecture-feasibility.md` §9.
  - Whether Android 10's seccomp/SELinux policy permits the `ptrace`/mmap
    behaviour the kernel-as-process tier needs is unmeasured (R2).
  - No measurement exists on the reference device (Snapdragon 855, Android 10).
    Everything recorded so far is from the x86-64 development host, except the
    AArch64 code paths, which run under `qemu-user`, not on real ARM hardware.
  - No **performance** measurement of Tier 1 vs Tier 0 exists yet. Tier 1 is
    expected to be slower than the interpreter at this stage (no register
    allocation, no chaining); that is a Phase 5 question, not a Phase 4 failure.
- See `docs/known-issues.md` for limitations.

---

## 4. Immediate next task

**Phase 4's core is implemented and verified. The next task is to close Phase 4
out and record it, then begin Phase 5 (performance measurement and
optimization).**

Phase 4 exit criterion: *a repeatable suite of small guest programs runs
correctly under the translator.* That is met — see §2 and §0c. Before declaring
Phase 4 complete, the remaining work is:

1. **Commit and push this milestone**, and watch CI go green (§2 has the local
   evidence; the remote run must be recorded here once it completes).
2. **Update `ROADMAP.md` and `CHANGELOG.md`** to mark Phase 4's items and record
   what changed.
3. **Extend the guest program suite** if time allows — a few more programs
   (nested loops, a branch inside a loop, a longer straight-line block that
   spans the fetch window) strengthen the exit criterion.

Then Phase 5 begins, and the single best first action there is:

- **Measure before optimizing.** Build a Tier-1-vs-Tier-0 timing harness on the
  existing benchmark infrastructure so every later optimization (register
  allocation, chaining, dispatch) has a fixed baseline to be judged against.
  Do not start optimizing until that baseline exists and is reproducible.

**The interpreter stays.** It is the oracle. Do not weaken it to make the
translator look correct. Phase 5 exit criteria are in `ROADMAP.md`.

Optional Phase 1 leftovers remain if wanted (unblocking the UML build, a CI
performance-trend baseline), but neither blocks Phase 5.

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
