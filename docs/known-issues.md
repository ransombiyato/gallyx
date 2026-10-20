# Known issues

Bugs, limitations and blocked work. Newest first within each section. An entry
is removed only when the issue is genuinely resolved, and the resolution is
noted in `CHANGELOG.md`.

---

## Blocked / cannot proceed without external input

| ID | Issue | Needs |
| --- | --- | --- |
| BLOCK-001 | No measurements from the reference target device (Snapdragon 855, Android 10). | Physical access to the device, or a comparable AArch64 Android host. Until then, all target-device claims are unverified. |
| BLOCK-002 | Development host is x86-64, so AArch64 code cannot be executed natively. | Mitigated by the cross + `qemu-user` pipeline (D-007). True AArch64 execution requires AArch64 hardware. |
| BLOCK-003 | The kernel-as-process tier cannot be committed to until the AArch64 UML port is run on the reference device. | AArch64 Android hardware. Until then the tier stays provisional (D-012) and no kernel-level code is written. |
| BLOCK-004 | The environment does not persist between sessions: a fresh container has no toolchain even if `build/` survives. | Run `scripts/bootstrap.sh`. Do not treat stale artifacts as evidence. See `STATUS.md` §2. |

---

## Limitations (by design, current phase)

| ID | Limitation | Notes |
| --- | --- | --- |
| LIM-002 | No syscall or ABI layer. | Phase 6. Guest `syscall` is not decoded. The syscall *inventory* now exists and is validated (`docs/syscalls/`), but no syscall is implemented; every entry is `not-started`. |
| LIM-003 | The translation cache **never invalidates**. | Self-modifying guest code runs stale translations. Deliberate boundary for the first version; precise invalidation is Phase 7. |
| LIM-004 | `qemu-user` validates correctness, not performance. | Never quote qemu numbers as performance. |
| LIM-005 | Benchmark harness measures host only. | `guest_execution.available = false` until a Tier 1/Tier 0 benchmark exists (D-009). No Tier-1 performance measurement exists yet. |
| LIM-006 | No Windows/macOS guest support. | Out of scope, permanently. |
| LIM-007 | Target device has no KVM and no root; the design must not assume either. | Enforced as a project rule. |
| LIM-008 | **Kernel-level compatibility is investigated, not implemented.** | The user-mode translator cannot provide `systemd` as PID 1, containers, or kernel-dependent services. The kernel-as-process tier is provisional and unvalidated on the target (D-011, D-012). |
| LIM-009 | AArch64 UML is an **out-of-tree** dependency (38 unmerged commits). | If the tier is adopted, it carries a maintenance liability (R7 in `docs/architecture-feasibility.md`). |
| LIM-010 | **No guest stack or memory operands.** | `push`/`pop`/`ret` and any ModRM memory operand are refused with an explicit error. Needs a guest address space (Phase 6). The translated subset is register/immediate ALU plus `mov`/`nop`/`hlt`/`jmp`/`jcc`. |
| LIM-011 | **No register allocation.** | Guest registers live in `GuestState` in memory and the generated code loads/stores them every instruction. Correct but slow; Phase 5. |
| LIM-012 | **No block chaining.** | Every block exit returns to the dispatcher for a hash lookup. Correct but slow; Phase 5. |
| LIM-013 | **Step-limit granularity differs between tiers.** | The interpreter steps one instruction at a time; the dispatcher runs whole blocks. On a step-limited run, tier 1 may stop a few instructions short of the limit (it never exceeds it). Only relevant to non-halting programs. |
| LIM-014 | `qemu-user` runs generated code, but it is **not the reference device**. | No measurement exists on the Snapdragon 855 / Android 10 target. |

---

## Correctness bugs

No open correctness bugs. Recorded history:

| ID | Symptom | Status |
| --- | --- | --- |
| BUG-001 | 64-bit `ADD` never set carry (`sum > mask` is impossible when the mask is `~0`). Found by the hardware differential. | Fixed (Phase 2). |
| BUG-002 | `Jcc` short jumps (`0x70..0x7F`) were decoded as `rel32`, producing wild targets. Found by the hardware differential. | Fixed (Phase 2). |
| BUG-003 | A block whose *first* instruction was unsupported was emitted as an empty fall-through, so the dispatcher looped forever. | Fixed (Phase 4); `translator.dispatcher_stops_at_unsupported` guards it. |
| BUG-004 | The code-fetch window shrank only to 15 bytes, so a program image shorter than that could not be translated. | Fixed (Phase 4); the CLI smoke test guards it. |
| BUG-005 | The dispatcher checked the step limit before running a block and then added the whole block count, so a 1,000,000-step run could retire 1,000,001 instructions. Found by the nested-loop differential. | Fixed (Phase 4); the step bound is now hard. |

When a bug is found, record: ID, symptom, minimal reproducer, affected commit,
severity (correctness/performance/cosmetic), status, and the test that guards
against regression.

---

## Toolchain and environment issues

| ID | Issue | Status |
| --- | --- | --- |
| ENV-001 | `g++-aarch64-linux-gnu` is required in addition to `gcc-aarch64-linux-gnu`; installing only the C cross-compiler breaks the cross test with "CXX compiler not found". | Resolved in `scripts/bootstrap.sh`, which installs both. |
| ENV-002 | ASan interacts with custom executable code caches. | Not yet encountered (no code cache). Expected in Phase 3; document per-test exclusions when they arise. |
| ENV-003 | CI prints a Node.js 20 deprecation annotation on every job: `actions/checkout@v4` is forced to run on Node.js 24. | Cosmetic; jobs pass. Resolve by bumping the pinned action version when a Node-24 action release is available. |

---

## Benchmarking defects

| ID | Issue | Status |
| --- | --- | --- |
| BENCH-001 | The Phase 1 benchmark harness reported a **fabricated** result. `-O2` hoisted the loop-invariant calibration call out of the repeat loop, so every timed region collapsed to ~0.00003 ns/op (~30 ps per iteration — far below one clock cycle) while the checksum stayed correct, so the error was silent. Detected by checking that total time scales with `--iterations`. | **Fixed.** An `asm` optimisation barrier now keeps the work between the clock reads, and the harness rejects an implausible timing (faster than one cycle per iteration) instead of reporting it. Regression test `bench.timing_not_eliminated`. |

---

## Rejected optimization candidates

Recorded so a future agent does not repeat a dead end.

| ID | Candidate | Verdict |
| --- | --- | --- |
| OPT-001 | Clear CF/OF/AF in `emit_flags_logical` by storing the AArch64 **zero register** (`strb wzr, …`) instead of `movz kT1, #0` + three `strb`s. | **Rejected — not encodable.** `strb w31, [x8, #7]` is an assembler error: the zero register is not a valid *source* for a store. Verified against `aarch64-linux-gnu-as`. No instruction saved. |
| OPT-002 | Invert PF in `emit_parity` with a logical-immediate `eor w3, w3, #1` instead of `movz kT1, #1` + `eor`. | **Deferred — not worth the cost.** The encoding exists (`eor w3, w3, #1` = `0x52000063`) but Hyperion has no logical-immediate encoder, so it needs a `BitMasks` decoder (~40 lines) plus tests for a single-instruction saving on a cold path. Low value; revisit only if PF generation shows up hot in a profile. |

---

## Open questions

| ID | Question | Impact |
| --- | --- | --- |
| Q-001 | Which upstream components, if any, should be reused rather than built? | Phase 1 decides; affects everything downstream. |
| Q-002 | Custom IR versus direct translation for the first code generator? | Phase 1/2; see D-004 for the current lean. |
| Q-003 | How to model x86 TSO memory ordering on weakly ordered AArch64? | Phase 7; affects correctness of multithreaded guests. |
| Q-004 | Does the AArch64 UML port build and boot under Android 10 on the Snapdragon 855, with no root and no KVM? | Decides whether the kernel-as-process tier is real (R1/R2). Blocks any kernel-level code. |
