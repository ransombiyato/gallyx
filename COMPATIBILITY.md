# COMPATIBILITY

Tracks implemented versus missing instruction and ABI features.

> **Phase 2 status:** a small, documented instruction subset is implemented and
> tested (§2). Everything else is explicitly rejected. There is no AArch64 code
> generation, no translation cache and no syscall layer yet.

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

## 2. Instruction subset — implemented (Phase 2)

This section is the **contract** the Phase 3 code generator must meet. It
describes what `src/core/decoder.cpp` accepts *today*, verified by
`tests/unit/test_decoder.cpp` and `tests/differential/`. Anything not listed
here is rejected with a specific `DecodeError` — the decoder never guesses.

### 2.1 Supported instructions

| Instruction | Encodings accepted | Operand sizes | Notes |
| --- | --- | --- | --- |
| `mov r, r/m` | `0x8B` | 32, 64 | reg ← reg/mem |
| `mov r/m, r` | `0x89` | 32, 64 | reg/mem ← reg |
| `mov r, imm` | `0xB8+rd` | 32, 64 | 32-bit imm; 64-bit form reads 8-byte imm |
| `add` | `0x01`, `0x03`, `0x81/0`, `0x83/0` | 32, 64 | r/m,r · r,r/m · imm32 · imm8(sx) |
| `or` | `0x09`, `0x0B`, `0x81/1`, `0x83/1` | 32, 64 | as above |
| `and` | `0x21`, `0x23`, `0x81/4`, `0x83/4` | 32, 64 | as above |
| `sub` | `0x29`, `0x2B`, `0x81/5`, `0x83/5` | 32, 64 | as above |
| `xor` | `0x31`, `0x33`, `0x81/6`, `0x83/6` | 32, 64 | as above |
| `cmp` | `0x39`, `0x3B`, `0x81/7`, `0x83/7` | 32, 64 | flags only; no write-back |
| `inc` | `0xFF/0` | 32, 64 | preserves CF |
| `dec` | `0xFF/1` | 32, 64 | preserves CF |
| `push r64` | `0x50+rd` | 64 | `rsp -= 8; [rsp] = r` |
| `pop r64` | `0x58+rd` | 64 | `r = [rsp]; rsp += 8` |
| `jmp rel32` | `0xE9` | — | absolute target = next rip + rel32 |
| `jmp rel8` | `0xEB` | — | short form; absolute target = next rip + rel8 |
| `jcc rel8` | `0x70..0x7F` | — | all 16 conditions; **short form only** |
| `nop` | `0x90` | — | |
| `hlt` | `0xF4` | — | stops execution (see §2.4) |
| `ret` | `0xC3` | — | modelled as "stop" (see §2.4) |

For the `0x81`/`0x83` immediate forms with a 64-bit operand, the immediate is
**sign-extended** to 64 bits (`imm32`→64, `imm8`→64), exactly as x86-64 does.
`0x80` (8-bit group 1) is rejected.

### 2.2 Addressing forms

`ModRM` mod=00/01/10 with rm≠100 (no SIB) and rm=100 (SIB) are supported:

- `[disp32]`, `[base]`, `[base + disp8]`, `[base + disp32]`
- `[base + index*scale + disp8/disp32]` with scale ∈ {1, 2, 4, 8}
- REX.R/REX.X/REX.B extend the reg/index/base fields to r8–r15.

**Not supported (rejected):** RIP-relative addressing (mod=00, rm=101) and the
8-bit (`0x88`/`0x8A`) forms.

### 2.3 Rejection behaviour (`DecodeError`)

| Error | Raised when |
| --- | --- |
| `kTruncated` | the instruction extends past the supplied bytes |
| `kUnknownOpcode` | opcode not in the supported set (incl. `0F`-map, `adc`/`sbb`) |
| `kUnsupportedPrefix` | `0x66`, `LOCK`, and other prefixes not modelled |
| `kUnsupportedOperand` | operand form outside the subset (e.g. 8-bit mov) |
| `kUnsupportedModrm` | ModRM/SIB combination not supported yet (e.g. RIP-relative) |

A rejected instruction must leave guest state **unchanged** and `rip`
**unmoved**; `tests/unit/test_interpreter.cpp` asserts this.

### 2.4 Deliberate simplifications

- `hlt` and `ret` both stop the interpreter and report `kHalted`. Real
  user-mode x86-64 would `#GP` on `hlt` and would pop a return address on
  `ret`. These are modelling choices for the Phase 2 test harness, recorded
  here so they are not mistaken for correct behaviour. They will be revisited
  in Phase 4 (block exits) and Phase 6 (signals).
- `ret` does **not** pop the stack.

### 2.5 Planned for Phase 2 but NOT implemented

These appeared in an earlier version of this table and remain unbuilt. They are
**not** covered by any test:

| Group | Instructions | Status |
| --- | --- | --- |
| `test` | `0x85` | ⬜ planned |
| Unary | `neg`, `not` | ⬜ planned |
| Shift/rotate | `shl`, `shr`, `sar` (imm and `CL`) | ⬜ planned |
| Multiply | `imul`, `mul` | ⬜ planned |
| Address | `lea` | ⬜ planned |
| Control flow | `call`, `ret` (real), `jcc rel32` (`0F 8x`) | ⬜ planned |
| Syscall | `syscall` (`0F 05`) | ⬜ planned (Phase 6) |
| Division | `div`, `idiv` | ⬜ planned (stretch) |
| 8-bit forms | `mov r8`, `add r8`, … | ⬜ planned |

**Out of Phase 2 scope entirely:** SIMD/SSE/AVX, x87, MMX, atomics,
`rep`-prefixed string ops, segmentation, and most system instructions. All are
rejected explicitly.

### 2.6 AArch64 translation coverage (Phase 3–4)

The translator lowers a subset of the decoder's subset. An instruction the
decoder accepts but the translator does not is refused with an explicit
`TranslateError`; nothing is miscompiled. Phase 4 adds whole-block translation
(`translate_block`) and a dispatcher (`TranslationCache`).

| Instruction | Translated? | Notes |
| --- | --- | --- |
| `mov` | ✅ | register and immediate, 32/64-bit; 32-bit zero-extends |
| `add`, `sub` | ✅ | register and immediate, 32/64-bit; full flag set |
| `and`, `or`, `xor` | ✅ | register and immediate; CF/OF/AF cleared |
| `cmp` | ✅ | flags only, no writeback |
| `inc`, `dec` | ✅ | CF preserved across the operation |
| `nop` | ✅ | no effect |
| `hlt` | ✅ | block exits with `kHalted`; `rip` not advanced |
| `jmp rel32` (`0xE9`) | ✅ | block exits with `kJmp`; writes target `rip` |
| `jmp rel8` (`0xEB`) | ✅ | block exits with `kJmp`; writes target `rip` |
| `jcc rel8` (`0x70..0x7F`) | ✅ | block exits with `kJcc`; writes target or fall-through `rip` |
| `push`, `pop`, `ret` | ⬜ | needs a guest stack / address space (Phase 6) |
| memory operands | ⬜ | Phase 6 (needs a guest address space) |

Verified by `tests/unit/test_translator.cpp`: scope tests on every host, a
single-instruction differential against the interpreter under `qemu-user` on
AArch64 (847 operand cases plus a fixed instruction set), **and whole-program
differentials** — a counted loop and taken/not-taken branches — that compare the
final `GuestState` and step count against `run_program`. The
`hyperion run --tier=1` CLI runs a complete program through generated AArch64
code and is required to agree with tier 0 (`tests/cli/cli_smoke.sh`).

---

## 3. Flag coverage

The interpreter implements **eager** flag computation (D-003) for six flags.
Lazy materialization is a Phase 5 optimization. Each flag below is verified
against real x86-64 hardware by `tests/differential/`, and the Phase 3
translator recomputes all six in generated AArch64 code (D-017), checked
against the interpreter by `tests/unit/test_translator.cpp`.

| Flag | Meaning | Status | Notes |
| --- | --- | --- | --- |
| CF | Carry / borrow | ✅ | set by add/sub; cleared by logic ops; preserved by inc/dec |
| ZF | Zero | ✅ | set by all modelled ALU ops |
| SF | Sign (bit 31/63) | ✅ | set by all modelled ALU ops |
| OF | Signed overflow | ✅ | set by add/sub/inc/dec; cleared by logic ops |
| PF | Parity of low byte | ✅ | `__builtin_parity`; no AArch64 equivalent, must be computed |
| AF | Auxiliary carry (bit 3) | ✅ | no AArch64 equivalent, must be computed |
| DF | Direction | ⬜ | only needed for string ops (Phase 7) |
| IF/TF/IOPL | System flags | ⬜ | not modelled; user-mode subset only |

`pack_flags`/`unpack_flags` map these to/from architectural RFLAGS bits
(CF=0, PF=2, AF=4, ZF=6, SF=7, OF=11), and bit 1 is always set as on real
hardware. Unmodelled bits are ignored on unpack and dropped on pack — this is
intentional and tested.

---

## 4. Register and state coverage

| State | Status | Notes |
| --- | --- | --- |
| GPR r64 (RAX…R15) | ✅ | full 64-bit values; index order matches x86-64 encoding |
| GPR 32-bit aliases | ✅ | 32-bit writes zero-extend to 64 (tested against hardware) |
| GPR 16/8-bit aliases | ⬜ | not modelled; 8-bit mov is rejected, no 16-bit prefix |
| RIP | ✅ | advanced per instruction; branches set an absolute target |
| RFLAGS | ✅ | six flags (CF/PF/AF/ZF/SF/OF), eager; see §3 |
| RSP / stack | 🟡 | `push`/`pop` work over `FlatMemory`; no guest stack mapping yet |
| XMM / SSE | ⬜ | Phase 7 |
| x87 / MMX | ⬜ | Phase 7, likely via reuse |
| FS/GS base, segment bases | ⬜ | Phase 6 (TLS) |
| MXCSR | ⬜ | Phase 7 |

`FlatMemory` is a single flat region used by the interpreter. It is **not** a
guest address space: there is no paging, no mapping of the guest stack, and no
`mmap`. That arrives in Phase 4/6.

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
