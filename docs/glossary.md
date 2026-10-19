# Glossary

Terminology used across Project HYPERION documentation.

| Term | Meaning |
| --- | --- |
| **AArch64** | The 64-bit ARM instruction set architecture; the HYPERION host ISA. |
| **ABI** | Application Binary Interface: calling conventions, register usage, struct layouts, syscall protocol. |
| **Basic block** | A straight-line sequence of instructions with one entry and one exit. The unit of translation in Phase 4. |
| **Block chaining** | Linking translation blocks so control flow transfers directly, avoiding a return to the dispatcher. |
| **Code cache / translation cache** | Memory holding generated host code, keyed by guest PC. |
| **Differential testing** | Executing the same program under two implementations (e.g. interpreter and code generator, or translator and native x86-64) and comparing observable state. The primary correctness strategy. |
| **Dispatch** | Locating and transferring control to the translated code for a guest PC. |
| **Guest** | The emulated x86-64 program and its architectural state. |
| **Host** | The AArch64 machine actually executing translated code. |
| **Hot path** | Code executed frequently enough to justify optimization or higher-tier compilation. |
| **HVI / IR** | Intermediate representation: an architecture-neutral form between x86-64 decoding and AArch64 code generation. |
| **Invalidation** | Discarding translated blocks when guest code or its mappings change (e.g. self-modifying code, `mprotect`). |
| **JIT** | Just-in-time compilation: generating host machine code at runtime. |
| **Lazy flags** | Deferring computation of the full x86 RFLAGS word until a guest instruction actually needs it. |
| **NZCV** | AArch64 condition flags: Negative, Zero, Carry, oVerflow. |
| **Partial register write** | Writing a sub-register (e.g. `AL`) while preserving the remaining bits of the full register. |
| **PC** | Program counter. `RIP` on x86-64; `PC` on AArch64. |
| **qemu-user** | QEMU's user-mode emulation, used here to execute cross-built AArch64 binaries on x86-64 for correctness validation only. |
| **RFLAGS** | The x86-64 flags register, including CF, PF, AF, ZF, SF, OF, DF. |
| **Tier** | A level of the execution engine: tier 0 = interpreter, tier 1 = straightforward JIT, tier 2 = optimized JIT. |
| **Translation block** | A decoded, translated unit of guest code (typically a basic block) with metadata and exits. |
| **TSO** | Total Store Order: x86's relatively strong memory model, versus AArch64's weaker model. |
| **W^X** | A memory-protection policy forbidding pages that are simultaneously writable and executable. Enforced on AArch64. |
| **XZR / WZR** | AArch64 zero registers. |
| **Syscall shim** | The component translating guest `syscall` invocations into host syscalls with argument/struct conversion. |
