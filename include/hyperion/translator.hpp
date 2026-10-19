// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// x86-64 -> AArch64 translation of the Phase 2 instruction subset.
//
// Scope of this first translator (Phase 3, increment 1) - deliberately small
// and stated rather than implied:
//   - Register and immediate operands only. A memory operand is reported as
//     unsupported by the translator, not silently miscompiled; memory needs an
//     address space that does not exist yet.
//   - MOV, ADD, SUB, AND, OR, XOR, CMP, INC, DEC, NOP, HLT.
//   - Control flow (JMP, Jcc, RET) and the stack (PUSH, POP) are Phase 4: they
//     need block exits and a translation cache to be meaningful.
//
// Guest registers live in the GuestState struct in memory; generated code
// loads and stores them. There is no guest-register allocation yet - that is a
// Phase 5 optimisation. Every x86 flag (CF/PF/AF/ZF/SF/OF) is computed in
// generated code from the AArch64 condition flags plus a couple of ALU
// identities, so the result is a genuine, independently-checkable translation
// rather than a call back into the interpreter. The interpreter remains the
// oracle (see DECISIONS.md D-003, D-014).
#ifndef HYPERION_TRANSLATOR_HPP
#define HYPERION_TRANSLATOR_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "hyperion/aarch64_encoder.hpp"
#include "hyperion/decoder.hpp"

namespace hyperion {

// Why a decoded instruction could not be translated. Explicit so a caller can
// distinguish "not in the translator's scope yet" from a real internal error.
enum class TranslateError {
    kNone = 0,
    kUnsupportedOpcode,  // control flow, stack: not in this increment
    kUnsupportedOperand, // memory operand, or an operand shape we do not emit
    kUnsupportedSize,    // operand width outside 32/64
    kInternal,           // a bug: the decoder produced something inconsistent
};

const char* to_string(TranslateError error);

struct Translation {
    TranslateError error = TranslateError::kNone;
    // AArch64 code for a block that is called as `uint64_t block(GuestState*)`
    // and returns kBlockOk or kBlockHalted (see below). Empty on error.
    std::vector<std::uint8_t> code;
    // Number of AArch64 instructions emitted (diagnostics/tests).
    std::size_t instruction_count = 0;

    bool ok() const { return error == TranslateError::kNone; }
};

// Value returned in x0 by a translated block.
enum class BlockResult : std::uint64_t {
    kOk = 0,     // instruction completed; guest state is updated
    kHalted = 1, // HLT: the program has stopped
};

// Translate one decoded x86-64 instruction into a self-contained AArch64 block
// with the signature `BlockResult block(GuestState*)`. The block reads and
// writes the guest state in place, updates rip for non-halting instructions,
// and returns kBlockOk or kBlockHalted.
//
// On an unsupported instruction the returned Translation carries an error and
// no code; nothing is silently emitted.
Translation translate_instruction(const Instruction& instruction);

} // namespace hyperion

#endif // HYPERION_TRANSLATOR_HPP
