# Prior art and reuse analysis

Phase 1 deliverable. Establishes what already exists, under what licence, and
what HYPERION should reuse versus build. Written **before** any engine code, as
required by `DECISIONS.md` D-002.

**Method.** Every licence and structural claim below was verified against the
upstream repository on 2026-10-19 (licence files fetched, source trees listed
via the GitHub API). Claims that could not be verified are marked as such.
Nothing here is quoted from memory. Stars are recorded as a rough activity
signal, not as an endorsement.

---

## 1. The landscape at a glance

| Project | Licence (verified) | Language | Direction | Relevance |
| --- | --- | --- | --- | --- |
| [FEX-Emu](https://github.com/FEX-Emu/FEX) | **MIT** (`LICENSE`) | C++ | x86-64 → AArch64 | **Directly comparable.** The closest thing to HYPERION's target, in production |
| [Box64](https://github.com/ptitSeb/box64) | **MIT** (`LICENSE`) | C | x86-64 → ARM64/RISC-V/LoongArch | Directly comparable; different technique (dynarec, no IR) |
| [QEMU](https://github.com/qemu/qemu) | **GPL-2.0** (`COPYING`) | C | many → many | Reference oracle; **not linkable** |
| [dynarmic](https://github.com/lioncash/dynarmic) | **0BSD** | C++ | ARM/AArch64 → x86-64/AArch64 | AArch64 *encoder* reference; opposite direction |
| [Zydis](https://github.com/zyantific/zydis) | **MIT** (`LICENSE`) | C | x86-64 decode/encode | Decoder candidate; excellent differential oracle |
| [Capstone](https://github.com/capstone-engine/capstone) | **BSD-style** (`LICENSE.TXT`: "Redistribution and use in source and binary forms…") | C | multi-arch disassembly | Disassembler; **not** a JIT-grade decoder |
| [LLVM](https://github.com/llvm/llvm-project) | **Apache-2.0 WITH LLVM-exception** (`LICENSE.TXT`) | C++ | compiler infra | Possible codegen backend; very large dependency |

---

## 2. Project-by-project

### 2.1 FEX-Emu — the closest comparable, and the hardest act to follow

MIT-licensed C++, actively developed, ~8.3k stars, not archived. Verified
source structure shows it already implements almost exactly the pipeline
`ROADMAP.md` describes:

| FEX component (verified path) | Corresponding HYPERION phase |
| --- | --- |
| `FEXCore/.../X86Tables`, `OpcodeDispatcher` | Phase 2 — x86-64 decode |
| `FEXCore/include/FEXCore/IR/IR.h` — a custom IR | Phase 2 — custom IR vs direct translation |
| `FEXCore/.../Interpreter` | Phase 2 — reference interpreter |
| `FEXCore/.../JIT/` — `ALUOps.cpp`, `AtomicOps.cpp`, `BranchOps.cpp`, `MemoryOps.cpp`, `VectorOps.cpp`, `MoveOps.cpp` | Phase 3 — AArch64 code generation |
| `CodeCache.cpp`, `LookupCache.cpp`, `DiskCache.cpp` | Phase 4 — translation cache |
| `VSyscall`, `LinuxEmulation` | Phase 6 — Linux ABI/syscalls |
| `ThunkLibs/` | Phase 9 — library forwarding |
| `Source/Windows` | (out of HYPERION scope) |

**Why this matters, stated plainly.** FEX already does what HYPERION's roadmap
describes, in a permissive licence, on the exact target architecture, and it is
deployed. HYPERION is **not** going to out-engineer FEX as a product. Anyone who
wants to run x86-64 Linux software on an ARM64 Android device should use FEX.

**What is nonetheless worth reusing or studying.** FEX's design choices are
free evidence: that a custom IR between decoder and JIT is workable at scale,
that a disk cache matters, that `ThunkLibs` is the right shape for library
forwarding, and that a reference interpreter alongside the JIT is practical.
Its IR header and thunk design are readable references for our own.

**Reuse risk.** FEX is MIT, so vendoring is legally fine, but its components are
tightly coupled to `FEXCore` internals. Lifting a subsystem out is not
realistic. Treat FEX as a **design reference and a differential oracle**, not as
a source of drop-in code.

### 2.2 Box64 — the same problem solved a different way

MIT-licensed C, ~5.7k stars, active. Box64 uses a **dynarec with no
intermediate representation** — direct x86-64 → host lowering — and reports
"5–10× faster than the interpreter alone" for its DynaRec on Arm, RISC-V and
LoongArch (from its `README.md`). It has a EuroSys 2026 paper on x86-64
emulation on RISC-V.

**Relevance.** Box64 is the strongest available evidence for the
"direct translation" branch of the custom-IR-vs-direct-translation question
(`DECISIONS.md` D-004). It demonstrates that direct lowering can be
competitive, which means our choice of IR must be justified on grounds other
than "an IR is obviously faster".

**Reuse.** MIT and legally reusable, but it is C and architected around a
different set of hosts and a different runtime model. As with FEX: reference
and oracle, not a code source.

### 2.3 QEMU — the correctness oracle, and a licence wall

GPL-2.0 (verified `COPYING`). QEMU's user-mode emulation (`qemu-aarch64`) is
already used by this project's cross-test pipeline, and `qemu-x86_64` is the
obvious independent oracle for differential-testing our decoder and interpreter
in Phase 2.

**Hard constraint.** GPL-2.0 is **incompatible** with linking into an
Apache-2.0 project. QEMU may be used **only as an external process/tool**, never
as vendored or linked code. This is recorded in `THIRD_PARTY.md` and enforced
here so no future agent links TCG sources into the core.

### 2.4 dynarmic — 0BSD, but the wrong direction

0BSD licensed (maximally permissive), C++. It JITs **from** ARM/AArch64 guest
code **to** a host. HYPERION needs the opposite: emit **to** AArch64. So
dynarmic's translation logic is not reusable.

**Why it is still listed.** Its AArch64 *emitter* is a readable, permissively
licensed example of how to encode AArch64 instructions correctly, and 0BSD
imposes essentially no obligations. Worth consulting in Phase 3 when we write
our own emitter.

### 2.5 Zydis and Capstone — decoders, with different jobs

- **Zydis** (MIT): a fast x86-64 decoder *and* encoder, designed for tooling
  and performance-sensitive use. Legally compatible.
- **Capstone** (BSD-style): a multi-architecture **disassembler** framework. Its
  focus is correct, human-readable disassembly across many architectures, not
  minimal-overhead per-instruction decode in a JIT hot path.

**Assessment.** Neither is an obvious drop-in for the Phase 2 decoder: the
directive explicitly calls for *custom* x86-64 decoding, and a JIT wants a
leaner decode path than a general tool provides. However, **Zydis is an
excellent differential oracle** for our decoder in Phase 2 — an independent
implementation to disagree with is exactly what differential testing needs.
Capstone serves the same purpose for spot-checking.

### 2.6 LLVM — powerful, and probably the wrong dependency

Apache-2.0 WITH LLVM-exception (verified), so legally compatible. Using LLVM as
a code-generation backend would give world-class optimisation for free.

**Assessment against this project.** LLVM is a very large dependency, it would
require building and shipping it for AArch64, and it would move the interesting
problem (instruction selection and register allocation for x86-64 semantics on
AArch64) out of HYPERION and into a compiler. The directive explicitly wants to
*investigate* code generation, register allocation and instruction scheduling.
Adopting LLVM as the backend would answer a different question. Recorded as
considered-and-declined for the core; it remains available for offline analysis
tooling.

---

## 3. Component-by-component reuse decision

| HYPERION component | Reuse? | Rationale |
| --- | --- | --- |
| x86-64 decoder | **Build** | The directive's stated research object. Zydis retained as an independent differential oracle |
| Intermediate representation | **Build** | The custom-IR question is a named research goal (D-004) |
| Reference interpreter | **Build** | It is the correctness oracle; a borrowed one would not be independent of the thing under test |
| AArch64 code generator | **Build** | The other stated research object. dynarmic consulted as an encoder reference |
| Executable-memory / code cache | **Build** | Small, and the W^X and cache-maintenance details are target-specific (D-010) |
| Syscall / ABI layer | **Build**, study FEX | `LinuxEmulation` is a design reference; too coupled to lift |
| Library forwarding | **Defer; study FEX `ThunkLibs`** | Phase 9. Reusing a proven design beats inventing one; revisit reuse then |
| Differential-testing oracles | **Reuse** | `qemu-x86_64`, Zydis, and native execution are the independent references |
| Benchmark harness | **Build** | Small, and must be ours to be trustworthy |

**Nothing GPL-2.0 is linked.** QEMU is an external tool only.

---

## 4. What the prior art de-risks

These are now **known-solved** problems rather than open research questions,
which lowers the risk on several roadmap phases:

1. x86-64 → AArch64 translation can be fast enough to be useful (FEX, Box64).
2. A custom IR between decoder and JIT is viable at scale (FEX).
3. Direct lowering without an IR is also viable (Box64) — so D-004 is a genuine
   trade-off, not a foregone conclusion.
4. Translation caching including a disk cache is worth having (FEX).
5. Library forwarding via generated thunks is the workable approach (FEX
   `ThunkLibs`).
6. Running a reference interpreter alongside a JIT is practical (FEX).

What the prior art does **not** settle: whether HYPERION's specific goals —
an AI-driven optimisation loop (Phase 8) and a measured comparison of design
choices — produce anything worth having. That is the actual research question,
and it is unaffected by how good FEX is.

---

## 5. Recommendation

**Build the core; reuse the oracles and the designs.**

1. **Build** the decoder, IR, interpreter, code generator and code cache. These
   *are* the research object. Reusing them would make the project pointless.
2. **Reuse as external tools**, never linked: `qemu-x86_64` and `qemu-aarch64`
   for differential testing, Zydis as a second decoder opinion, LLVM tools for
   object inspection. All are invoked as processes.
3. **Reuse as design references**: FEX's IR and thunk design, Box64's dynarec
   structure, dynarmic's AArch64 emitter. Read, do not vendor.
4. **Revisit reuse at Phase 9** for library forwarding, where a proven design is
   more valuable than novelty.
5. **State the positioning honestly** in the README: HYPERION is an experimental
   research platform for measuring translation and optimisation techniques. It is
   not a competitor to FEX, and users who want to run software should use FEX.

This is recorded as **D-013** in `DECISIONS.md`.

---

## 6. AArch64 UML: an attempted measurement

The scope correction added an optional Phase 1 item: cross-build the AArch64
UML port to test the "kernel-as-process" idea (running a real Linux kernel as a
user-mode process, with no KVM and no root). An attempt was made. It did **not**
succeed, and the reason is worth recording.

**What was tried.** `zalexdev/linux-um-arm64` (branch `um-arm64`, Linux 7.2.0-rc4)
was shallow-cloned and configured:

```bash
make ARCH=um SUBARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
make ARCH=um SUBARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j4
```

The port is real: `arch/arm64/um/` contains `os-Linux/registers.c`, `ptrace.c`,
`setjmp_aarch64.S`, `stub_segv.c`, a vDSO and `asm/`, and there is a dedicated
`arch/um/configs/arm64_defconfig`. Configuration succeeded.

**What happened.** The build compiled a large number of objects and then failed:

```
././include/linux/compiler_attributes.h:44:41: error: expected ')' before '__attribute__'
/usr/aarch64-linux-gnu/include/stdlib.h:673:6: error: expected ',' or ';' before ')' token
make[4]: *** [scripts/Makefile.build:289: arch/arm64/um/os-Linux/registers.o] Error 1
```

**Root cause.** This is **not** an arm64-port defect and **not** a HYPERION
defect. `include/linux/compiler_attributes.h` does not define
`__attribute_alloc_size__`; it is expected to come from glibc's `sys/cdefs.h`.
This Debian 13 glibc only defines it when a `__has_attribute(alloc_size)`
probe succeeds, and the kernel's forced-include sequence does not provide that
probe. The same failure occurs when the exact compiler flags are replayed
against plain `#include <stdlib.h>`.

**The decisive test.** Stock **x86-64** UML (`ARCH=um SUBARCH=x86_64`) was
built from the same tree on the same host, and failed with the *identical*
error at the identical header line. A failure that reproduces on the fully
upstream, fully supported subarchitecture is a host-toolchain/upstream
incompatibility, not anything specific to the AArch64 work.

**Conclusion.** The AArch64 UML port is **plausible but unverified**. Its build
is blocked by a host glibc/kernel-header mismatch that also blocks stock UML.
Resolving it (older glibc, a distribution kernel toolchain, or a kernel-side
patch) is out of scope for Phase 1 and was not pursued, because the R1 tier is
optional and speculative. No claim is made that the port works.

This is recorded rather than omitted because "we tried the optional item and
here is precisely why it did not complete" is a more useful handoff than
silence, and because the negative result is itself informative: the blocker is
environmental and shared with upstream, so it is unlikely to be an artefact of
the port.

---

## 7. Open questions deferred to later phases

| Question | Phase | Note |
| --- | --- | --- |
| Custom IR or direct translation? | 2 | Box64 vs FEX is the evidence; decide with a prototype, not by argument |
| Does Zydis's decode rate meet the JIT's needs? | 2 | Measure; do not assume |
| Is a disk translation cache worth its complexity? | 4 | FEX says yes; measure on the target device |
| Which upstream does library forwarding best? | 9 | FEX `ThunkLibs` vs Box64's wrapped libraries |
| Could LLVM be a *second* backend for comparison? | 5+ | Interesting for the Phase 8 optimisation lab, not for the core |

---

## 7. Verification log

Facts checked on 2026-10-19:

- Licence files fetched and read: FEX `LICENSE` (MIT), Box64 `LICENSE` (MIT),
  QEMU `COPYING` (GPL-2.0), Zydis `LICENSE` (MIT), Capstone `LICENSE.TXT`
  (BSD-style), LLVM `LICENSE.TXT` (Apache-2.0 WITH LLVM-exception).
- Repository metadata via the GitHub API: licence, language, stars, archived
  status for each project above.
- Source-tree structure via the GitHub API: FEX `FEXCore/Source/Interface/Core`
  (verified to contain `Interpreter`, `JIT`, `CodeCache.cpp`, `LookupCache.cpp`,
  `DiskCache.cpp`, `OpcodeDispatcher`, `X86Tables`, `VSyscall`), FEX
  `FEXCore/include/FEXCore/IR/IR.h`, FEX `FEXCore/.../JIT/` (verified to contain
  `ALUOps.cpp`, `AtomicOps.cpp`, `BranchOps.cpp`, `MemoryOps.cpp`,
  `VectorOps.cpp`, `MoveOps.cpp`), FEX top level (verified `ThunkLibs`).
- Box64 `README.md` read for its own description of DynaRec and host targets.
- `dynarmic/dynarmic` returned 404; the project was located as
  `lioncash/dynarmic` (0BSD) via repository search. **Not** verified beyond
  licence, stars and language.

**Not verified** (and therefore not claimed): benchmark numbers from any
project, FEX's or Box64's performance on the reference device, and any
statement about FEX's internal quality beyond the file structure listed above.
