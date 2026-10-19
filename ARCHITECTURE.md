# ARCHITECTURE

This document describes the intended system design and, importantly, **which
parts exist today**. Phase 0 implements only the shaded (existing) components.

Legend: `[ ]` planned · `[x]` implemented · `[~]` partially implemented

---

## 1. Problem statement

Translate and execute x86-64 Linux user-mode binaries on an AArch64 Linux
host, correctly first and fast second. The guest and host share an OS kernel
and (usually) a page size, but differ in:

| Property | x86-64 guest | AArch64 host |
| --- | --- | --- |
| Instruction encoding | Variable length (1–15 bytes) | Fixed 4 bytes |
| General registers | 16 (RAX…R15), partial-width aliasing | 31 (X0…X30) + SP/XZR |
| Flags | One RFLAGS with arithmetic flags | NZCV; no PF/AF; conditionals are branches/selects |
| Address model | 64-bit flat | 64-bit flat |
| Memory ordering | TSO (strong) | Weak |
| Syscall ABI | `syscall`, RAX number, RDI/RSI/RDX/R10/R8/R9 args | `svc #0`, X8 number, X0–X5 args |
| W^X | Historically relaxed | Enforced |

The translation problem is dominated by three mismatches: **flags**, **partial
register writes**, and **variable-length decoding**. Each is treated explicitly
in the design below.

---

## 2. Layered design

```
                    +---------------------------------------+
                    |            CLI (src/cli)      [x]     |
                    +---------------------------------------+
                                      |
   +----------------------------------+----------------------------------+
   |                                                                     |
   |  [x] core: version, host probe, (later) logging, config             |
   |                                                                     |
   +---------------------------------------------------------------------+
        |                 |                  |                |
   [ ] decoder      [ ] IR / semantics   [ ] AArch64        [ ] runtime
   x86-64 bytes  ->  typed basic blocks  codegen         ->  cache, memory,
        |                 |                  |                dispatch
        +--------+--------+---------+--------+----------------+
                 |                 |                 |
          [ ] interpreter   [ ] JIT tiers      [ ] syscall/ABI layer
          (oracle)          (tier 1, 2)        (guest <-> host)
                                                       |
                                              [ ] library forwarding
                                              [ ] graphics forwarding (opt.)
```

### Component boundaries

| Component | Responsibility | Must not |
| --- | --- | --- |
| Decoder | bytes → instructions; never executes | know about AArch64 |
| IR / semantics | architecture-neutral meaning | allocate registers or host addresses |
| Interpreter | execute IR; correctness oracle | be optimized at the cost of clarity |
| Code generator | IR → AArch64 machine code | decide *what* to translate |
| Runtime / cache | code memory, dispatch, invalidation | interpret semantics |
| Syscall layer | guest↔host ABI translation | implement host kernel policy |
| CLI | user interface | contain translation logic |

The rule that keeps this tractable: **the decoder never knows about AArch64,
and the code generator never knows about x86-64.** They meet only at the IR.

---

## 3. Guest state representation

The x86-64 architectural state the translator must model:

```c
struct GuestState {          // illustrative; not yet implemented
    uint64_t rax, rbx, rcx, rdx;      // plus rsi, rdi, rbp, rsp
    uint64_t r8..r15;
    uint64_t rip;
    uint64_t rflags;                  // materialized lazily where possible
    // xmm0..xmm15, mmx, x87 later; segment bases; fs/gs base
};
```

**Flags strategy.** AArch64 has no parity or auxiliary flag, and x86 arithmetic
flags do not map one-to-one onto NZCV. The plan is *lazy flag materialization*:
arithmetic produces NZCV plus a small "flag source" descriptor, and the full
RFLAGS word is computed only when a guest instruction actually reads flags in a
way that needs it. Phase 2 starts with eager flags (simpler, obviously
correct) and the optimization is deferred to Phase 5 with differential tests
proving equivalence. Recorded as decision D-003.

**Partial register writes.** Writing `AL` must preserve the upper 56 bits of
`RAX`. Phase 2 will model each register as a full 64-bit value and perform
read-modify-write for partial widths; later phases may track "known high bits".

---

## 4. Execution model and tiers

```
        guest PC
           |
           v
   +-----------------+   miss    +-------------------+
   |  dispatch/hash  |---------->| decode -> IR      |
   |  (block lookup) |           | translate -> code |
   +-----------------+           +-------------------+
           | hit                          |
           v                              v
   +-----------------+          +-------------------+
   |  tier-1 code    |          |  tier-2 optimized |
   |  (fast, simple) |          |  (profiled)       |
   +-----------------+          +-------------------+
```

- **Tier 0 (Phase 2):** reference interpreter. Always available, used as the
  differential oracle for everything above it.
- **Tier 1 (Phase 3–4):** straightforward AArch64 code generation, one
  translation block per basic block, minimal register allocation.
- **Tier 2 (Phase 5+):** optimized blocks for hot paths — better register
  allocation, constant propagation, dead-code elimination, block chaining.

Tier promotion is driven by measured execution counts, never speculation.
Every tier must produce bit-identical guest state; this is enforced by tests.

---

## 5. Translation cache and invalidation

Planned design (Phase 4):

- Fixed-size, hash-indexed lookup keyed by guest PC (plus a generation tag).
- Code stored in page-aligned arenas allocated with `mmap`; on AArch64 the
  arena is written with `PROT_WRITE`, then made executable with
  `PROT_READ|PROT_EXEC` and flushed with `__builtin___clear_cache`.
- Invalidation: a mapping from guest page → set of blocks, so self-modifying
  code or `mprotect` invalidates precisely.
- **W^X is assumed on AArch64** (probed at runtime, see `host_probe`). The
  design never assumes a page is simultaneously writable and executable.

---

## 6. Syscall and ABI layer (Phase 6)

Guest `syscall` instructions are intercepted and translated. The layer owns:

- argument register marshalling (`RDI,RSI,RDX,R10,R8,R9` → `X0..X5`, `RAX`→`X8`);
- structure layout differences (e.g. `stat`, `epoll_event`, `iovec` where
  widths differ);
- memory mapping (`mmap`/`munmap`/`mprotect`) coordination with the code cache;
- signals, threads (`clone`), futexes, and `rseq`.

Compatibility is claimed only via the feature matrix in
[`COMPATIBILITY.md`](COMPATIBILITY.md) backed by integration tests. A handful
of working programs does **not** constitute "Linux compatibility".

---

## 7. Reuse strategy (see Phase 1)

HYPERION will not reimplement mature, license-compatible components where
reuse is safer. Phase 1 produces a written comparison of FEX-Emu, Box64,
QEMU TCG and relevant decoder/compiler projects, their licences and their
trade-offs. `THIRD_PARTY.md` is the licence inventory. No third-party code is
vendored until that analysis is committed.

---

## 8. Safety boundaries

- Guest code runs **only** inside the translator, in-process, with no elevated
  privileges. The translator never requires root or KVM.
- Generated code is produced by our own code generator from validated IR; the
  translator does not `exec` guest binaries directly.
- Host library forwarding is opt-in and allow-listed (Phase 9).
- Details and the full threat model: [`SECURITY.md`](SECURITY.md).

---

## 9. Directory-to-component mapping

| Directory | Component | Phase introduced |
| --- | --- | --- |
| `src/core/` | shared services (version, host probe, logging) | 0 |
| `src/cli/` | user interface | 0 |
| `src/decoder/` | x86-64 decoding | 2 |
| `src/ir/` | intermediate representation | 2 |
| `src/interp/` | reference interpreter | 2 |
| `src/codegen/` | AArch64 code generation | 3 |
| `src/runtime/` | code cache, dispatch, memory | 4 |
| `src/syscall/` | Linux ABI layer | 6 |
| `tests/`, `benchmarks/` | verification | 0 |

Directories are created only when their phase begins, to keep the tree honest
about what exists.
