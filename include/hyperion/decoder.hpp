// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// x86-64 instruction decoding for the Phase 2 subset.
//
// Design (see DECISIONS.md D-014 and COMPATIBILITY.md):
//   - Decoding is explicit and total: a byte sequence either decodes to a
//     known instruction or is rejected with a reason. There is no "best
//     effort" path that silently produces a wrong instruction.
//   - Operands are decoded into a small, uniform form (register / immediate /
//     memory address) so the interpreter and the later code generator consume
//     the same representation and cannot disagree about operand shape.
//   - Only the documented subset is accepted. Anything else - an unknown
//     opcode, a prefix we do not model, a LOCK, a 16-bit operand size - is an
//     error, not a guess. Unsupported must fail loudly (CONTRIBUTING.md §3).
#ifndef HYPERION_DECODER_HPP
#define HYPERION_DECODER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "hyperion/guest_state.hpp"

namespace hyperion {

// Why a byte sequence was rejected. Kept explicit so tests can assert the
// precise reason rather than "it failed".
enum class DecodeError {
    kNone = 0,
    kTruncated,          // ran out of bytes mid-instruction
    kUnknownOpcode,      // opcode not in the supported subset
    kUnsupportedPrefix,  // prefix outside the modelled set (e.g. LOCK, 0x66)
    kUnsupportedOperand, // operand form outside the subset (e.g. RIP-relative)
    kUnsupportedModrm,   // ModRM/SIB combination not supported yet
    kTooLong,            // exceeded the 15-byte architectural limit
};

const char* to_string(DecodeError error);

// Operand size. Phase 2 implements 32- and 64-bit forms only; 8- and 16-bit
// forms are rejected rather than approximated.
enum class OperandSize : std::uint8_t {
    k8 = 1,
    k16 = 2,
    k32 = 4,
    k64 = 8,
};

// How an operand is written by an instruction.
enum class OperandKind : std::uint8_t {
    kRegister,
    kImmediate,
    kMemory,
};

// A decoded memory operand: base + index*scale + disp.
//
// Note the deliberate restriction: base and index are 64-bit general-purpose
// registers with no RIP-relative form. RIP-relative addressing is common in
// real code and is a Phase 2 follow-up; rejecting it now keeps the decoder
// honest about what it actually handles.
struct MemoryOperand {
    bool has_base = false;
    Gpr base = Gpr::kRax;
    bool has_index = false;
    Gpr index = Gpr::kRax;
    std::uint8_t scale = 1;
    std::int64_t disp = 0;
    // Effective address size; Phase 2 is always 64-bit (0x67 prefix rejected).
    OperandSize address_size = OperandSize::k64;
};

struct Operand {
    OperandKind kind = OperandKind::kRegister;
    OperandSize size = OperandSize::k64;
    Gpr reg = Gpr::kRax;   // when kind == kRegister
    std::uint64_t imm = 0; // when kind == kImmediate (zero-extended)
    MemoryOperand mem;     // when kind == kMemory
};

// The supported instruction mnemonics. This list is the contract: COMPATIBILITY.md
// lists exactly these, and the decoder accepts nothing else.
enum class Opcode : std::uint8_t {
    kMov,     // MOV  r/m, r  |  MOV r, r/m  |  MOV r, imm
    kAdd,     // ADD  r/m, r
    kSub,     // SUB  r/m, r
    kCmp,     // CMP  r/m, r
    kAnd,     // AND  r/m, r
    kOr,      // OR   r/m, r
    kXor,     // XOR  r/m, r
    kInc,     // INC  r/m
    kDec,     // DEC  r/m
    kNop,     // NOP
    kRet,     // RET
    kHlt,     // HLT
    kJmp,     // JMP rel32
    kJcc,     // Jcc rel8 (condition in the low nibble of the opcode)
    kPush,    // PUSH r64
    kPop,     // POP  r64
    kSyscall, // SYSCALL (0F 05): guest Linux system call
};

const char* to_string(Opcode opcode);

// Condition codes for Jcc. The encoding value is the low nibble of the opcode
// (0x70..0x7F).
enum class Condition : std::uint8_t {
    kO = 0x0,
    kNo = 0x1,
    kB = 0x2,
    kAe = 0x3,
    kE = 0x4,
    kNe = 0x5,
    kBe = 0x6,
    kA = 0x7,
    kS = 0x8,
    kNs = 0x9,
    kP = 0xA,
    kNp = 0xB,
    kL = 0xC,
    kGe = 0xD,
    kLe = 0xE,
    kG = 0xF,
};

const char* to_string(Condition condition);

// A fully decoded instruction. `length` is the number of bytes consumed, so
// the caller advances RIP by exactly that much.
struct Instruction {
    Opcode opcode = Opcode::kNop;
    Condition condition = Condition::kE; // valid only for kJcc
    std::uint64_t address = 0;           // address of the first byte
    std::uint8_t length = 0;             // bytes consumed
    Operand dst;                         // destination / first operand
    Operand src;                         // source / second operand
    bool has_src = false;
    bool rex_w = false; // 64-bit operand size was selected
};

struct DecodeResult {
    DecodeError error = DecodeError::kNone;
    Instruction instruction{};

    bool ok() const { return error == DecodeError::kNone; }
    explicit operator bool() const { return ok(); }
};

// Decode one instruction from `code` (at most `size` bytes) located at guest
// address `address`. Never reads past `size` bytes.
DecodeResult decode(const std::uint8_t* code, std::size_t size, std::uint64_t address);

// Human-readable form, used in test failure messages. Not a full assembler:
// it renders exactly the operand shapes this decoder supports.
std::string to_string(const Instruction& instruction);

} // namespace hyperion

#endif // HYPERION_DECODER_HPP
