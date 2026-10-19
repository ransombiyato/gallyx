# COMPATIBILITY

Tracks implemented versus missing instruction and ABI features.

> **Nothing is implemented yet.** This file defines the vocabulary and the
> tracking rules so that coverage is recorded honestly from Phase 2 onward.

Legend: ✅ implemented + tested · 🟡 implemented, untested · 🧪 prototype · ❌ unsupported (explicit) · ⬜ planned

---

## 1. Tracking rules

1. A feature is ✅ only when it has **dedicated automated tests** that pass.
2. A feature implemented but not yet covered by tests is 🟡 and must be listed
   in `docs/known-issues.md`.
3. Unsupported instructions must **fail explicitly** at translation time with a
   clear diagnostic. Silent misinterpretation is a correctness bug of the
   highest severity.
4. Every row states the phase in which it is expected to land.

---

## 2. Instruction subset — Phase 2 target

The Phase 2 subset is deliberately small and explicit.

| Group | Instructions | Status | Phase |
| --- | --- | --- | --- |
| Data movement | `mov r64, imm64`, `mov r64, r64`, `mov r64, [mem]`, `mov [mem], r64` | ⬜ | 2 |
| 32-bit forms | `mov r32, imm32`, `mov r32, r32` (zero-extending) | ⬜ | 2 |
| Arithmetic | `add`, `sub`, `and`, `or`, `xor`, `cmp`, `test` (64/32-bit) | ⬜ | 2 |
| Unary | `inc`, `dec`, `neg`, `not` | ⬜ | 2 |
| Shift/rotate | `shl`, `shr`, `sar` (by immediate and by `CL`) | ⬜ | 2 |
| Multiply | `imul` (2- and 3-operand forms), `mul` | ⬜ | 2 |
| Address computation | `lea` | ⬜ | 2 |
| Stack | `push`, `pop` | ⬜ | 2 |
| Control flow | `jmp`, `jcc` (all 16 conditions), `call`, `ret` | ⬜ | 2 |
| Misc | `nop` | ⬜ | 2 |
| Syscall entry | `syscall` (dispatch stub; full ABI in Phase 6) | ⬜ | 2/6 |
| Division | `idiv`, `div` | ⬜ | 2 (stretch) |

**Out of Phase 2 scope:** SIMD/SSE/AVX, x87, MMX, atomics, `rep`-prefixed
string ops, segmentation, most system instructions. These must be rejected
explicitly.

---

## 3. Flag coverage

| Flag | Meaning | Status | Notes |
| --- | --- | --- | --- |
| CF | Carry | ⬜ | no direct AArch64 equivalent for all cases |
| ZF | Zero | ⬜ | maps to NZCV `Z` |
| SF | Sign | ⬜ | maps to NZCV `N` |
| OF | Overflow | ⬜ | maps to NZCV `V` |
| PF | Parity (low byte) | ⬜ | no AArch64 equivalent; must be computed |
| AF | Auxiliary carry | ⬜ | no AArch64 equivalent; must be computed |
| DF | Direction | ⬜ | needed for string ops (Phase 7) |
| IF/TF/IOPL | System | ⬜ | user-mode subset only |

Phase 2 implements **eager** flag computation (D-003). Lazy materialization is
a Phase 5 optimization.

---

## 4. Register and state coverage

| State | Status | Notes |
| --- | --- | --- |
| GPR r64 (RAX…R15) | ⬜ | full 64-bit values |
| GPR 32/16/8-bit aliases | ⬜ | partial-write semantics, incl. `AH/BH/CH/DH` |
| RIP | ⬜ | tracked per block |
| RFLAGS | ⬜ | eager in Phase 2 |
| RSP / stack | ⬜ | guest stack managed in guest address space |
| XMM / SSE | ⬜ | Phase 7 |
| x87 / MMX | ⬜ | Phase 7, likely via reuse |
| FS/GS base, segment bases | ⬜ | Phase 6 (TLS) |
| MXCSR | ⬜ | Phase 7 |

---

## 5. Linux ABI / syscall coverage

Phase 6. Nothing implemented. The feature matrix will be maintained here with
per-syscall status and the integration test that covers it.

| Area | Status | Phase |
| --- | --- | --- |
| Syscall number + argument marshalling | ⬜ | 6 |
| `exit`, `exit_group` | ⬜ | 6 |
| `read`, `write`, `close`, `open`, `openat` | ⬜ | 6 |
| `mmap`, `munmap`, `mprotect`, `brk` | ⬜ | 6 |
| `stat`/`fstat`/`lstat`/`*statat` (struct layout differences) | ⬜ | 6 |
| `arch_prctl` (FS/GS base, TLS) | ⬜ | 6 |
| `clone`, `futex`, threading | ⬜ | 6 |
| Signals | ⬜ | 6 |
| `epoll`, `poll`, `select` | ⬜ | 6 |
| `io_uring`, `rseq`, `vDSO` | ⬜ | 6/7 |

---

## 6. Host forwarding

| Capability | Status | Phase |
| --- | --- | --- |
| Native library forwarding (allow-listed) | ⬜ | 9 |
| Graphics API forwarding (optional) | ⬜ | 9 |

---

## 7. How to update this file

When implementing a feature: set its status, add the covering test name, and
commit the change in the same milestone. When discovering an unsupported
instruction during testing: add a ❌ row and a `docs/known-issues.md` entry.
