# THIRD_PARTY

Dependency and licence inventory.

> Policy (D-002): no third-party code is vendored until the Phase 1 reuse
> analysis is committed. Policy (D-005): the core library has **no third-party
> runtime dependency**.

---

## 1. Core runtime dependencies

**None.** `hyperion_core` links only against the C++ standard library. This is
deliberate: the target device (Android 10, no root, no package manager) must be
able to build and run the translator without a dependency ecosystem.

---

## 2. Build and development dependencies

These are required to build or test HYPERION but are **not** linked into the
shipped core. They are obtained from the distribution package archive
(`scripts/bootstrap.sh`).

| Dependency | Purpose | Licence | Linked into core? |
| --- | --- | --- | --- |
| CMake ≥ 3.20 | Build system | BSD-3-Clause | No |
| Ninja | Build backend | Apache-2.0 | No |
| GCC 14 (`g++`) | Host compiler | GPL-3.0-or-later (with runtime exception) | No (runtime libstdc++ exception applies) |
| Clang 19 | Alternative host compiler | Apache-2.0 WITH LLVM-exception | No |
| LLVM 19 (`llvm-mc`, `llvm-objdump`) | Instruction encoding/decoding cross-checks (Phase 2+) | Apache-2.0 WITH LLVM-exception | No |
| `aarch64-linux-gnu-gcc/g++` | AArch64 cross-compiler for validation | GPL-3.0-or-later (with runtime exception) | No |
| GNU binutils (aarch64) | Cross-linker/assembler | GPL-3.0-or-later | No |
| QEMU user-mode (`qemu-aarch64`, `qemu-x86_64`) | Execute cross-built and guest binaries for validation | GPL-2.0 | No |
| GDB (multiarch) | Debugging | GPL-3.0-or-later | No |
| clang-format / clang-tidy | Formatting and static analysis | Apache-2.0 WITH LLVM-exception | No |

---

## 3. Licence compatibility

The project is licensed **Apache-2.0**. Considerations for future reuse:

| Upstream candidate | Licence | Compatibility note |
| --- | --- | --- |
| FEX-Emu | MIT | Compatible; Phase 1 analysis required before any reuse |
| Box64 | MIT | Compatible; Phase 1 analysis required |
| QEMU TCG | GPL-2.0 | **Incompatible with Apache-2.0 redistribution if linked.** Usable only as an external *tool* for validation, not as vendored code |
| LLVM (as a library) | Apache-2.0 WITH LLVM-exception | Compatible if used as a library |
| Zydis (x86 decoder) | MIT | Compatible; evaluation in Phase 1 |
| Capstone | BSD-3-Clause | Compatible; evaluation in Phase 1 |

**QEMU is used strictly as an external validation tool.** Its GPL-2.0 code is
never linked into HYPERION. This is recorded here so no future agent
accidentally links QEMU source into the core.

The Phase 1 analysis will finalize the reuse-vs-build boundary and record the
decision in `DECISIONS.md`.

---

## 4. Licence of HYPERION itself

Apache License 2.0. See [`LICENSE`](LICENSE). Every source file carries an
`SPDX-License-Identifier: Apache-2.0` header.
