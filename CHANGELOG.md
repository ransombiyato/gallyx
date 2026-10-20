# CHANGELOG

Meaningful changes by milestone. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions are pre-release.

---

## [Unreleased] — Phase 6 (guest descriptor table + read/close/lseek)

### Fixed — clang build (CI `build & test (clang)` and `ASan/UBSan`)
- `src/core/syscall.cpp` indexed the descriptor table with a signed `int64_t`
  (`files[fd]`), which clang's `-Wsign-conversion` rejects under
  `HYPERION_WARNINGS_AS_ERRORS=ON`. gcc's `-Wconversion` did not flag it, so
  the local build passed while CI failed. The bounds check and index conversion
  now live in a single `lookup_stream(files, fd)` helper, so no signed value
  reaches `vector::operator[]`. `scripts/dev.sh clang-build` added so this
  class of failure is catchable locally before pushing.

### Added — guest descriptor table (third syscall slice)
- `MinimalSyscallHandler` now owns a **guest descriptor table**
  (guest fd → host `FILE*`), so `read`/`close`/`lseek` operate on descriptors
  the handler controls rather than on raw host fds. The table starts with
  fd 0/1/2 on the process's stdin/stdout/stderr; `adopt(guest_fd, stream)`
  attaches an already-open host stream under another guest fd. A guest fd that
  is not in the table is `-EBADF`, so a guest can never name an arbitrary host
  descriptor. The handler never `fclose`s a borrowed stream.
- `read`(0): copies from an adopted stream into guest memory in 4 KiB chunks,
  returning a short count at end-of-stream (as `read(2)` does), `-EBADF` for a
  bad descriptor and `-EFAULT` for an out-of-range guest buffer.
- `close`(3): drops the guest descriptor (does not close the borrowed stream).
- `lseek`(8): `SEEK_SET`/`SEEK_CUR`/`SEEK_END` over an adopted stream;
  `-ESPIPE` for a non-seekable stream, `-EINVAL` for a bad `whence`.
- `docs/syscalls/inventory.tsv` moves `read`(0), `close`(3) and `lseek`(8) from
  `not-started` to `implemented`; `docs/syscalls/` regenerated.
- New errno constants `kEperm`, `kEnoent`, `kEio`, `kEspipe`.
- Tests: `syscall.read_copies_from_adopted_stream_into_guest_memory`,
  `syscall.read_of_bad_fd_is_ebadf`, `syscall.read_out_of_range_buffer_is_efault`,
  `syscall.read_short_read_at_end_of_stream`,
  `syscall.close_drops_the_descriptor_but_not_the_stream`,
  `syscall.close_of_bad_fd_is_ebadf`, `syscall.lseek_repositions_an_adopted_stream`,
  `syscall.lseek_end_and_cur_whence_work`,
  `syscall.lseek_bad_whence_is_einval_and_bad_fd_is_ebadf`,
  `syscall.adopt_refuses_an_occupied_descriptor`,
  `syscall.open_is_still_unimplemented`.

### Not added (stated, not implied)
- `open`/`openat` remain **unimplemented**: a guest can read inherited
  descriptors but cannot open host files. The descriptor table maps to `FILE*`,
  so `O_NONBLOCK`, `O_APPEND`, `pread`/`pwrite`, `fcntl` and `dup` are also not
  expressible yet.

---

## [Unreleased] — Phase 6 (guest address space + memory syscalls)

### Added — guest address space (DECISIONS.md D-023)
- `GuestAddressSpace` (`include/hyperion/address_space.hpp`,
  `src/core/address_space.cpp`): guest virtual memory as a set of regions, each
  with a base, length and `MemPerm` (read/write/exec), with page-granular
  permission checks on every access. `map_image`, `map_anonymous`, `read`,
  `write`, `protect`, `unmap`, `find_region`, `range_is_free`, and a reserved
  `[heap_base, heap_end)` window so the heap and `mmap` allocations cannot
  overlap. `set_brk` maps the pages a grow touches.
- Unit tests in `tests/unit/test_address_space.cpp`: map/read/write, permission
  denial on a read-only region, `brk` growth, and page-granular range checks.

### Added — memory syscalls (inventory status → `implemented`)
- `mmap`(9) (anonymous private mappings only), `mprotect`(10), `munmap`(11) and
  `brk`(12) are implemented in `MinimalSyscallHandler` over a caller-supplied
  `GuestAddressSpace`. `docs/syscalls/inventory.tsv` moves all four from
  `not-started` to `implemented`; `docs/syscalls/` regenerated.
- A memory syscall's return value and its *implemented* status are tracked
  separately (`MemResult`): a file-backed `mmap` returns `-ENOSYS` **and** is
  counted in `RunResult::unimplemented_syscalls`, so a partial implementation is
  never reported as support. Without an address space the four return `-ENOSYS`
  and count as unimplemented, rather than pretending success.
- Tests: `syscall.mmap_anonymous_returns_a_mapping_that_is_usable`,
  `syscall.mmap_zero_length_is_einval`,
  `syscall.mmap_file_backed_is_enosys_not_a_wrong_mapping`,
  `syscall.mprotect_and_munmap_round_trip`, `syscall.brk_returns_the_new_break`,
  `syscall.memory_syscalls_without_an_address_space_are_enosys`,
  `syscall.is_minimal_syscall_reports_the_new_set`.
- End-to-end integration: `syscall.mmap_end_to_end_under_the_interpreter` runs a
  real guest program (mmap → store through the returned pointer) under Tier 0;
  `syscall.mmap_end_to_end_under_generated_code_stops_at_memory_operand` pins
  the honest Tier 1 boundary — the mmap syscall is handled, then the block stops
  at the memory-operand store because the code generator is register-only.

### Fixed
- `mmap`'s `fd`/`offset` are no longer inspected for `MAP_ANONYMOUS`: the kernel
  ignores them, so the handler does too, instead of returning `-ENOSYS` for a
  legal anonymous mapping that happened to pass a real fd.
- `set_brk` now actually maps the heap memory it grows into (a design gap in the
  first address-space draft).

---

## [Unreleased] — Phase 5 (performance optimizations 3–6)

### Added — flag liveness (BENCHMARKS.md §4.2.3)
- `translate_block` is now three passes: decode the block, run backward flag
  liveness over it, then emit. Each ALU instruction computes only the flags read
  before being overwritten; `live_out` starts at all flags at the block exit
  (flags persist across blocks, so there is no cross-block analysis).
- INC/DEC no longer emit an explicit CF save/restore: `flags_written()` excludes
  CF for them, so CF is preserved structurally.
- Generated code for the fixed workload fell 584 → 352 bytes (−39.7%); the
  size-guard program fell 148 → 90 words. No wall-clock claim under `qemu-user`.
- New deterministic guard: `translator.dead_flags_are_not_computed`.

### Added — direct-mapped dispatch (BENCHMARKS.md §4.2.4)
- `TranslationCache` consults a 4096-entry direct-mapped table (low 12 bits of
  the guest PC) before the hash map. The map stays authoritative, so a collision
  is a safe fall-through, never a wrong block.
- Measured dispatch hit rate on the fixed workload: 199,998 hits / 3 misses
  (99.9985%); counters exposed in the benchmark JSON.
- New test: `translation_cache.direct_map_collision_falls_back_to_map`.

### Added — ALU immediate folding (BENCHMARKS.md §4.2.5)
- `add/sub/cmp reg, imm` with an immediate that fits the AArch64 12-bit ADD/SUB
  immediate form folds into one ADDS/SUBS-immediate, instead of materialising the
  constant first. Applied only when AF is dead (AF needs `b` in a register).
- Size-guard program fell 90 → 88 words.
- New test: `translator.alu_immediate_is_folded`.

### Added — guest-register allocation within a block (BENCHMARKS.md §4.2.6)
- Pass 1.5 ranks guest registers by use count in the block and maps the hottest to
  callee-saved host registers (x19..x28), saved/restored around the block with
  `stp`/`ldp`. `mov`, the ALU ops and `inc`/`dec` operate directly on the
  allocated register when both operands are allocated.
- A **profitability guard** declines allocation when the prologue/epilogue would
  cost more than the saved loads. The first version allocated every touched
  register and *regressed* the tiny benchmark loop (364 → 388 bytes); the guard
  fixes that (356 bytes on, 364 off).
- New `--wide` workload (12-instruction body, 11 guest registers) added to
  `hyperion_tier_bench` because the narrow loop cannot show the win. Result:
  620 → 608 bytes and 10.70 → 9.05 ns/guest-instruction (RA off → on),
  reproducible across runs.
- New tests: `translator.differential_register_allocation_many_registers`,
  `translator.differential_register_allocation_immediates`.

### Fixed — halted `rip` was the block start (BENCHMARKS.md §4.2.7)
- A block ending in `HLT` (or `RET`) returned `kHalted` without writing `rip`, so
  the reported halted `rip` was the block's first address instead of the `HLT`
  address. Fixed by emitting `rip = insn.address` on the halt path. Found by the
  new many-register differential test (the first program to end a
  multi-instruction block in `HLT` and compare full state).

### Added — repo-local tool cache (`scripts/toolcache.sh`)
- The container wipes `/usr` between sessions but the repository directory
  persists, so the AArch64 emulator and cross toolchain are cached under
  `tools/cache/` (git-ignored). `scripts/bootstrap.sh` populates the cache;
  `scripts/toolcache.sh restore` reinstates the toolchain offline after a reset.
  The benchmark and cross-test scripts pick up the cached `qemu-aarch64-static`
  automatically. Recovery path verified by deleting the sysroot and restoring it.

### Notes
- All four optimizations were accepted on **deterministic** evidence (generated
  code size, dispatch counters) plus a relative Tier 1 time signal; `qemu-user`
  wall-clock is too noisy for absolute claims, so no hardware speedup is claimed.
  The interpreter remains the correctness oracle and the differential suite must
  pass unchanged.

---

## [Unreleased] — Phase 6 (first syscall slice)

### Added — guest syscall layer (first slice)
- `include/hyperion/syscall.hpp`, `src/core/syscall.cpp`: the `SyscallHandler`
  interface (guest state in, result written to `rax`) and
  `MinimalSyscallHandler`, which implements `write`(1), `exit`(60) and
  `exit_group`(231) and returns `-ENOSYS` for every other number.
- The x86-64 `syscall` instruction (`0F 05`) is decoded as `Opcode::kSyscall`.
- Both execution tiers route syscalls through the handler: the interpreter
  handles it inline; the translator ends the block with
  `BlockResult::kSyscall` and the dispatcher runs the handler, so a syscall is
  never skipped or executed with wrong guest state.
- `RunResult::unimplemented_syscalls` records how many `-ENOSYS` calls the
  program made, so an unimplemented call is visible, never silent.
- Error convention matches Linux: failures return a negative errno in `rax`
  (e.g. `-EBADF`, `-EFAULT`); a guest buffer outside the mapped region yields
  `-EFAULT` rather than a host crash.
- 11 new tests (`tests/unit/test_syscall.cpp`): decoding, handler semantics
  (good/bad fd, out-of-range pointer, exit code, unknown number), interpreter
  integration, and a Tier 1 differential that runs generated AArch64 under
  `qemu-user`.

### Changed — syscall inventory
- `docs/syscalls/inventory.tsv`: `write`, `exit` and `exit_group` moved from
  `not-started` to `implemented`; the seven generated files regenerated. Every
  other entry remains `not-started`.

### Notes
- Exactly three syscalls are implemented. This is a first slice, not general
  Linux compatibility; see `docs/known-issues.md` LIM-002.

---

## [Unreleased] — Phase 6 (syscall inventory)

### Added — evidence-based syscall inventory
- `docs/syscalls/inventory.tsv`: the canonical, one-row-per-syscall data source
  (number, name, tier, status, purpose, evidence).
- `docs/syscalls/kernel-table-x86_64-6.12.tsv`: the pinned Linux v6.12 x86-64
  syscall table (`arch/x86/entry/syscalls/syscall_64.tbl`), reduced to the 375
  `common`/`64` entries.
- `docs/syscalls/U1.md`–`U4.md`, `E1.md`–`E3.md`: the seven required inventory
  files, generated from `inventory.tsv`. Distribution: U4=39, U3=75, U2=32,
  U1=14, E1=8, E2=100, E3=107 (375 total).
- `docs/syscalls/README.md`: tier criteria, the pinned-kernel policy and the
  evidence policy.
- `tools/gen_syscall_inventory.py`: validates the inventory against the pinned
  table (missing, duplicate, misnumbered, mis-tiered, x32, invalid status) and
  regenerates the seven Markdown files.
- `scripts/trace-syscalls.sh`: the reproducible strace sweep behind the
  `trace:N` evidence values. It is honest about its limits: a small x86-64-host
  sample, treated as a lower bound; unmeasured entries are `provisional`.
- Wired into the suite as `syscall.inventory_valid` and into CI as the
  `inventory` job, which also fails if the generated Markdown is stale.

### Notes
- The inventory records *classification and priority*, not implementation. Every
  entry is `not-started`; no syscall is claimed as working.

---

## [Unreleased] — Phase 5

### Added — performance measurement
- `benchmarks/tier_bench.cpp` (target `hyperion_tier_bench`): runs one fixed
  guest loop through Tier 0 (interpreter) and Tier 1 (translator), reporting
  median ns and ns per retired guest instruction with an IQR spread, using the
  shared methodology (one discarded warm-up, repeated samples, a correctness
  check on the final register). Tier 1 fields are reported **unavailable** on a
  host that cannot execute generated AArch64 code, rather than fabricated.
- `scripts/bench-tier-qemu.sh`: reproducible Tier 0 vs Tier 1 measurement on an
  AArch64 host (under `qemu-user` on a development machine).

### Recorded — first baseline (BENCHMARKS.md §4.2)
- Fixed loop, 600,002 retired guest instructions, median of 7 under
  `qemu-aarch64`: Tier 0 = 491.9 ns/guest-ins, Tier 1 = 38.4 ns/guest-ins,
  ratio 0.078 (Tier 1 ~12.8× faster). Documented explicitly as a *relative*
  signal between Hyperion's own tiers, not a hardware performance claim.

### Changed — Optimization 1: parity (PF) fast path (BENCHMARKS.md §4.2.1)
- `emit_parity` (`src/core/translator.cpp`) rewritten from a per-bit
  accumulation loop (19 emitted AArch64 instructions, including setup) to a
  shifted-`EOR` XOR-fold (6). x86 PF semantics unchanged.
- New encoder primitive `Encoder::eor_shifted` (EOR with a shifted register),
  with encodings verified against `aarch64-linux-gnu-as` for all four shift
  types. A first attempt wrongly set bit 21; the new encoder test caught it.
- Measured by an interleaved A/B against the pre-change commit on the same host:
  generated code for the fixed workload **904 → 644 bytes (−28.8%,
  deterministic)**, Tier 1 wall time **−17.5%**, Tier 1/Tier 0 ratio
  0.0817 → 0.0733. Correctness re-verified by the full differential suite under
  `qemu-aarch64` (99 passed, 0 failed).

### Added — deterministic translation-size diagnostics
- `TranslationCache::total_code_bytes()` and `CachedBlock::code_bytes`, plus a
  `tier1_translator.generated_code_bytes` field in the benchmark JSON. These are
  deterministic, so they act as a regression guard where `qemu-user` timing
  noise (IQR of several percent) would hide a code-size regression.
- Test `translator.translated_code_size_is_bounded` pins the block count and an
  upper bound on generated words for a fixed guest loop.

### Changed — Optimization 2: dead `rip` store elimination (BENCHMARKS.md §4.2.2)
- `emit_one` gained a `write_rip` parameter. Translated blocks emit body
  instructions with `write_rip=false` and write `rip` exactly once, at the
  block exit; nothing inside a block reads `rip`, so the per-instruction
  `movz`+`movk`+`str` was dead. The single-instruction translator keeps
  `write_rip=true` (its caller reads `rip` back).
- Generated code for the fixed workload **644 → 584 bytes (−9.3%)**; Tier 1 wall
  time −4% in an interleaved A/B. Delta fully accounted for (5 body instructions
  × 3 instructions). The differential suite compares the final `rip`, so a
  misplaced write fails hard.
- Tightened `translator.translated_code_size_is_bounded` from a loose `< 200`
  word bound to an upper bound at the current count (148 words), so any future
  growth fails CI while a reduction is still allowed.

### Docs
- Split `STATUS.md` phase sections correctly (two headings both said "Phase 3";
  the earlier block is Phase 2). Corrected the stale claim that no translator
  exists.

---

## [Unreleased] — Phase 4

### Added — basic dynamic binary translator
- `include/hyperion/translation_cache.hpp`, `src/core/translation_cache.cpp`:
  `TranslationCache`, the Tier 1 dispatcher. It looks up the block for
  `state.rip`, translates it on first sight, executes it, and repeats until the
  program halts, an unsupported instruction is reached, or a guest-instruction
  step limit is hit. It **never executes an instruction the translator refused**:
  an unsupported instruction stops the run with `RunStatus::kUnsupported` and
  leaves `rip` on the offending instruction.
- `translate_block` (`src/core/translator.cpp`) lowers a whole basic block:
  it decodes from a guest PC until a terminator (`jmp`/`jcc`/`hlt`), the end of
  the buffer, or an untranslatable instruction. It returns the generated code,
  a `BlockExit` (fall-through / jmp / jcc / halted), a target/fall-through hint,
  and the number of guest instructions covered. The authoritative next PC is
  always the `rip` field the generated block writes.
- `include/hyperion/run.hpp`: `RunStatus`/`RunResult` extracted from
  `interpreter.hpp` so both execution tiers report through the same types and
  can be compared directly.

### Added — `hyperion run`
- `src/cli/main.cpp`: `hyperion run <file> [--tier=0|1] [--max-steps=N] [--dump]`
  executes a raw x86-64 code blob through the reference interpreter (tier 0) or
  the AArch64 translator (tier 1). On a host that cannot execute generated
  AArch64 code, tier 1 falls back to the interpreter and says so on stderr.

### Added — decoder
- `0xEB` (`jmp rel8`): short unconditional jump, the companion to the
  `0x70..0x7F` short `jcc`. Decoder test `decoder.jmp_short_rel8`.

### Added — tests
- `tests/unit/test_translator.cpp`: block-formation scope tests (terminator,
  unsupported cut, buffer-end cut), whole-program differentials against
  `run_program` (counted loop; taken/not-taken branches across flag states), and
  dispatcher tests (stops at unsupported; infinite loop → step limit).
- `tests/unit/test_translation_cache.cpp`: the cache refuses to run when the
  host cannot execute generated code; caches blocks and agrees with the
  interpreter on AArch64.
- `tests/cli/cli_smoke.sh` + CTest entry `cli.smoke`: runs the real CLI through
  both tiers and requires them to agree. The cross pipeline runs it under
  `qemu-user` with `HYPERION_EMULATOR`, requiring tier 1 to have translated a
  non-zero number of blocks.

### Fixed
- **Infinite loop on an unsupported first instruction.** A block whose first
  instruction was unsupported was emitted as a zero-instruction fall-through
  block, so the dispatcher re-entered the same PC forever. `translate_block` now
  reports the error when the block would be empty.
- **Short program images failed to translate.** The dispatcher's code-fetch
  window shrank only to 15 bytes, so a 13-byte blob could not be read. The window
  now shrinks to a single byte.
- **The dispatcher could exceed the step limit.** It checked the limit before
  running a block and then added the whole block's count, so a 1,000,000-step
  run could retire 1,000,001 instructions. A block that does not fit the
  remaining budget is now not started, so the bound is hard.

### Added — more guest programs
- `translator.differential_nested_loops` (two backward branches) and
  `translator.differential_branch_inside_loop` (taken/not-taken exits that
  reconverge and alternate). Both compare against `run_program` and also assert
  an independently computed final value, so agreement cannot be a shared
  mistake.

---

## [Unreleased] — Phase 3

### Added — x86-64 to AArch64 translator (increment 1)
- `include/hyperion/translator.hpp`, `src/core/translator.cpp`:
  `translate_instruction` lowers one decoded x86-64 instruction to a
  self-contained AArch64 block with the signature `BlockResult block(GuestState*)`.
  Supported: `mov`, `add`, `sub`, `and`, `or`, `xor`, `cmp`, `inc`, `dec`,
  `nop`, `hlt` with 32/64-bit register or immediate operands. Guest registers
  live in the `GuestState` struct; generated code loads/stores them.
- All six x86 flags (CF/PF/AF/ZF/SF/OF) are computed **in generated code** from
  AArch64 NZCV plus two ALU identities, never delegated to the interpreter, so
  the differential test is an independent check (D-017).
- Out-of-scope instructions are refused with an explicit `TranslateError`
  (`jmp`/`jcc`/`ret`/`push`/`pop` → `kUnsupportedOpcode`; memory operands →
  `kUnsupportedOperand`) rather than miscompiled.
- `tests/unit/test_translator.cpp`: scope tests run on every host; the
  differential tests execute generated blocks under `qemu-user` on AArch64 and
  compare every register and all six flags against the interpreter, including an
  847-case operand sweep over carry/overflow/parity boundaries. On x86-64 they
  report **skip**, not a false pass.

### Added — AArch64 encoder and executable memory
- `include/hyperion/aarch64_encoder.hpp`, `src/core/aarch64_encoder.cpp`:
  `a64::Encoder`, a checked AArch64 emitter for the operations the Phase 3
  translator needs first — add/sub/and/orr/eor (register and immediate forms),
  cmp/cmp-imm, mov/movz/movk/mov-imm64, b/b.cond/ret/nop, and label-based
  branch fixups. It throws on an out-of-range register or immediate instead of
  emitting a wrong word. **Every encoding is verified against GNU as/objdump**;
  the expected words in `tests/unit/test_aarch64_encoder.cpp` are binutils
  output, not values written from memory.
- `include/hyperion/exec_mem.hpp`, `src/core/exec_mem.cpp`: `ExecMem`, a
  page-aligned region that is written read-write, then made read-execute with an
  instruction-cache clear (`W^X`, per `DECISIONS.md` D-016). Writing after
  `finalize()` is refused rather than silently corrupting code.
- `tests/unit/test_exec_mem.cpp`: on AArch64 (including under `qemu-user` in
  CI) these generate and **execute** real code — `add x0,x0,x1; ret` and a
  `1..n` summation loop — and check the results. On x86-64 they report **skip**,
  not a false pass.

### Fixed
- `include/hyperion/decoder.hpp`: the `kJcc` comment said "rel32"; the decoder
  implements rel8 short forms only. Comment corrected to match the code and
  `COMPATIBILITY.md` §2.

### Decisions
- D-014 (decoder contract is the subset boundary), D-015 (checked encoder),
  D-016 (W^X executable memory), D-017 (flags recomputed in generated code).

---

## [Unreleased] — Phase 2

### Added — guest state, decoder, reference interpreter
- `include/hyperion/guest_state.hpp`, `src/core/guest_state.cpp`: the guest
  register file (16 GPRs, index order matching the x86-64 ModRM/REX encoding),
  `rip`, and a `Flags` struct with `pack_flags`/`unpack_flags` mapping to and
  from architectural RFLAGS bits.
- `include/hyperion/decoder.hpp`, `src/core/decoder.cpp`: an x86-64 decoder for
  a documented subset — MOV/ADD/SUB/AND/OR/XOR/CMP/INC/DEC/PUSH/POP/JMP/Jcc
  (rel8)/NOP/HLT — with REX, ModRM and SIB support for
  `base + index*scale + disp`. Every unsupported byte sequence is **rejected**
  with a specific `DecodeError` (`kUnsupportedPrefix`, `kUnsupportedModrm`,
  `kUnknownOpcode`, `kTruncated`); it never guesses.
- `include/hyperion/interpreter.hpp`, `src/core/interpreter.cpp`: a reference
  interpreter over a `FlatMemory` region, implementing the ALU flag model (CF,
  PF, AF, ZF, SF, OF), effective-address computation, and x86-64's 32-bit
  zero-extension rule. This is the **correctness oracle** for Phase 3.
- 38 new unit test cases: `test_guest_state.cpp` (7), `test_decoder.cpp` (19),
  `test_interpreter.cpp` (16, including four that run whole programs).

### Added — program-image runner and the subset contract
- `run_program(memory, state, max_steps)`: runs a program image until HLT/RET,
  a fault, or a step limit, and returns the final state, retired-instruction
  count and a reason. A convenience layer over `step()` with no new semantics;
  the default step limit exists so a non-halting guest program is *reported*,
  not allowed to hang the suite.
- Tests run a real `dec`/`jnz` loop (sum 1..10 = 55) and cross-check the result
  against native x86-64; a second test proves an infinite loop reports
  `kStepLimit`; a third proves execution stops at the first unsupported
  instruction rather than running past it.
- `COMPATIBILITY.md` §2 rewritten from a *plan* into a *specification*: it now
  documents exactly what the decoder accepts (opcodes, encodings, operand
  sizes, addressing forms, `DecodeError` cases) and lists the planned-but-
  unbuilt instructions in a separate section so the plan is not mistaken for
  working code. §3 and §4 updated to the real flag and register coverage.

### Added — hardware differential test
- `tests/differential/test_differential.cpp`: compares the interpreter against
  **real x86-64 hardware** by executing the same operation natively (inline
  asm, flags captured through `pushfq`) and in the interpreter, across many
  operand values and flag inputs. 8 cases. This is the Phase 2 acceptance test:
  the interpreter is only trusted because an independent implementation agrees.
- Added `HYPERION_SKIP`: the harness can now report a test as **skipped**
  (not executed), separately from passing. On non-x86-64 hosts the differential
  cases SKIP rather than falsely reporting coverage — on AArch64 the suite
  reports `0 passed, 8 skipped`.

### Fixed — two bugs found by the differential test
- `alu::add` never set CF for 64-bit operands: the check `sum > mask` is
  impossible when `mask == ~0`, so the carry test could not fire. Corrected to
  `(sum < ua) || (sum > mask)`.
- `Jcc` short jumps (`0x70..0x7F`) were decoded with a rel32 displacement
  instead of rel8, producing wild branch targets. The rel32 forms are
  `0x0F 0x80..0x8F` and are still unsupported.

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
