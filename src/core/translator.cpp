// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// x86-64 -> AArch64 translation of the Phase 2 subset (see translator.hpp for
// the exact scope). The generated block has the signature
// `BlockResult block(GuestState*)` and updates the guest state in place.
//
// Design notes:
//   - Guest registers live in memory (the GuestState). Generated code loads
//     and stores them; there is no register allocation yet (Phase 5).
//   - x86 flags are *computed*, not delegated: AArch64's ADDS/SUBS set N/Z/C/V,
//     which map onto x86 SF/ZF/CF/OF directly; AF and PF are derived with two
//     ALU identities documented below. This is what makes the translation
//     independently checkable against the interpreter rather than a re-entry
//     into it.
//   - The x8 register holds the GuestState pointer. It is caller-saved in the
//     AAPCS64, so the block needs no prologue/epilogue.
#include <cstddef>
#include <type_traits>

#include "hyperion/translator.hpp"

namespace hyperion {

namespace {

// AArch64 registers used by a translated block. x8 is the state pointer; the
// rest are scratch. All are caller-saved, so no save/restore is required.
constexpr a64::Reg kState = 8; // GuestState*
constexpr a64::Reg kA = 9;     // operand A (destination value)
constexpr a64::Reg kB = 10;    // operand B (source value)
constexpr a64::Reg kR = 11;    // result
constexpr a64::Reg kT0 = 12;   // scratch
constexpr a64::Reg kT1 = 13;   // scratch
constexpr a64::Reg kT2 = 14;   // scratch
constexpr a64::Reg kT3 = 15;   // scratch

// Offsets into GuestState. Generated code hard-codes these, so they are tied
// to the struct with offsetof/static_assert: a layout change becomes a compile
// error here instead of silently corrupting translated code.
constexpr unsigned kRipOffset = offsetof(GuestState, rip);
constexpr unsigned kCfOffset = offsetof(GuestState, flags) + offsetof(Flags, cf);
constexpr unsigned kPfOffset = offsetof(GuestState, flags) + offsetof(Flags, pf);
constexpr unsigned kAfOffset = offsetof(GuestState, flags) + offsetof(Flags, af);
constexpr unsigned kZfOffset = offsetof(GuestState, flags) + offsetof(Flags, zf);
constexpr unsigned kSfOffset = offsetof(GuestState, flags) + offsetof(Flags, sf);
constexpr unsigned kOfOffset = offsetof(GuestState, flags) + offsetof(Flags, of);

static_assert(std::is_trivially_copyable_v<GuestState>,
              "generated code relies on GuestState being a plain memory image");

constexpr unsigned gpr_offset(Gpr r) {
    return 8u * static_cast<unsigned>(r); // 0..120
}

// gpr[] must be first in GuestState for the gpr_offset() arithmetic to hold.
static_assert(offsetof(GuestState, gpr) == 0, "gpr[] must be first in GuestState");
static_assert(offsetof(GuestState, gpr) + 8 * kGprCount == offsetof(GuestState, rip),
              "gpr[] must be immediately followed by rip");

void load_gpr(a64::Encoder& e, Gpr r, a64::Reg dst) {
    e.ldr(a64::Width::k64, dst, kState, gpr_offset(r));
}

void store_gpr(a64::Encoder& e, a64::Reg src, Gpr r) {
    e.str(a64::Width::k64, src, kState, gpr_offset(r));
}

// Parity of the low byte of `src`, into `dst` (0 = odd, 1 = even).
//
// x86 PF is set when the number of set bits in the low byte is even. XOR-folding
// the eight bits gives the odd/even bit; inverting it gives PF. We fold by
// summing the bits (the low bit of the sum is their XOR) and then XOR with 1.
// This is deliberately simple; Phase 5 can replace it with a table or CNT.
void emit_parity(a64::Encoder& e, a64::Reg dst, a64::Reg src) {
    e.ubfx(a64::Width::k32, kT2, src, 0, 1);
    e.mov_reg(a64::Width::k32, dst, kT2);
    for (unsigned i = 1; i < 8; ++i) {
        e.ubfx(a64::Width::k32, kT2, src, i, 1);
        e.adds(a64::Width::k32, dst, dst, kT2); // dst accumulates 0..8
    }
    e.ubfx(a64::Width::k32, dst, dst, 0, 1); // low bit = XOR of the eight bits
    e.movz(a64::Width::k32, kT3, 1);
    e.eor(a64::Width::k32, dst, dst, kT3); // PF = 1 - parity_bit
}

// Store SF, ZF, CF and OF from the AArch64 condition flags, which must have
// been set by a preceding ADDS (is_sub=false) or SUBS (is_sub=true). `t2` must
// hold (a ^ b ^ result) for the AF identity; `result` is used for parity.
void emit_flags_from_nzcv(a64::Encoder& e, bool is_sub, a64::Reg t2, a64::Reg result) {
    e.cset(a64::Width::k32, kT1, a64::Cond::kEq); // ZF = Z
    e.strb(kT1, kState, kZfOffset);
    e.cset(a64::Width::k32, kT1, a64::Cond::kMi); // SF = N
    e.strb(kT1, kState, kSfOffset);
    // x86 CF is the carry-out: for add it is "carry set" (CS); for subtract it
    // is "borrow" (CC, i.e. no borrow), the standard ADDS/SUBS mapping.
    e.cset(a64::Width::k32, kT1, is_sub ? a64::Cond::kCc : a64::Cond::kCs);
    e.strb(kT1, kState, kCfOffset);
    e.cset(a64::Width::k32, kT1, a64::Cond::kVs); // OF = V
    e.strb(kT1, kState, kOfOffset);
    // AF: carry out of bit 3. For both ADD and SUB, AF == bit 4 of (a ^ b ^ r).
    e.ubfx(a64::Width::k32, kT1, t2, 4, 1);
    e.strb(kT1, kState, kAfOffset);
    // Parity is computed last: it clobbers the condition flags, which are no
    // longer needed once the csets above have run.
    emit_parity(e, kT1, result);
    e.strb(kT1, kState, kPfOffset);
}

// Logical ops (AND/OR/XOR) clear CF, OF and AF, and set SF/ZF/PF from the
// result. There is no NZCV involvement.
void emit_flags_logical(a64::Encoder& e, a64::Width w, a64::Reg result) {
    e.cmp_imm(w, result, 0); // sets N (sign) and Z from the result
    e.cset(a64::Width::k32, kT1, a64::Cond::kEq);
    e.strb(kT1, kState, kZfOffset);
    e.cset(a64::Width::k32, kT1, a64::Cond::kMi);
    e.strb(kT1, kState, kSfOffset);
    e.movz(a64::Width::k32, kT1, 0);
    e.strb(kT1, kState, kCfOffset);
    e.strb(kT1, kState, kOfOffset);
    e.strb(kT1, kState, kAfOffset);
    emit_parity(e, kT1, result);
    e.strb(kT1, kState, kPfOffset);
}

// Load a source operand into `dst`. Only registers and immediates are in scope
// for this increment.
TranslateError load_source(a64::Encoder& e, const Operand& src, a64::Reg dst, int bits) {
    switch (src.kind) {
    case OperandKind::kRegister:
        load_gpr(e, src.reg, dst);
        if (bits == 32) {
            // A 32-bit operand use sees only the low 32 bits, and a 32-bit MOV
            // must zero-extend into the destination (the x86-64 rule).
            e.mov_reg(a64::Width::k32, dst, dst);
        }
        return TranslateError::kNone;
    case OperandKind::kImmediate: {
        const std::uint64_t value = (bits == 32) ? (src.imm & 0xFFFFFFFFULL) : src.imm;
        e.mov_imm64(dst, value);
        return TranslateError::kNone;
    }
    case OperandKind::kMemory:
        return TranslateError::kUnsupportedOperand;
    }
    return TranslateError::kInternal;
}

void emit_return(a64::Encoder& e, BlockResult result) {
    e.movz(a64::Width::k64, 0, static_cast<std::uint16_t>(result));
    e.ret();
}

// Emit `rip = instruction.address + instruction.length`.
void emit_advance_rip(a64::Encoder& e, const Instruction& insn) {
    e.mov_imm64(kT0, insn.address + insn.length);
    e.str(a64::Width::k64, kT0, kState, kRipOffset);
}

} // namespace

const char* to_string(TranslateError error) {
    switch (error) {
    case TranslateError::kNone:
        return "none";
    case TranslateError::kUnsupportedOpcode:
        return "unsupported opcode";
    case TranslateError::kUnsupportedOperand:
        return "unsupported operand";
    case TranslateError::kUnsupportedSize:
        return "unsupported operand size";
    case TranslateError::kInternal:
        return "internal error";
    }
    return "?";
}

Translation translate_instruction(const Instruction& insn) {
    Translation out;
    a64::Encoder e;

    // Validate operand size and the destination shape up front, so the opcode
    // switch can assume a register destination at 32 or 64 bits.
    const bool is32 = insn.dst.size == OperandSize::k32;
    if (insn.opcode != Opcode::kNop && insn.opcode != Opcode::kHlt && insn.opcode != Opcode::kJmp &&
        insn.opcode != Opcode::kJcc && insn.opcode != Opcode::kRet) {
        if (insn.dst.size != OperandSize::k32 && insn.dst.size != OperandSize::k64) {
            out.error = TranslateError::kUnsupportedSize;
            return out;
        }
        if (insn.dst.kind == OperandKind::kMemory) {
            out.error = TranslateError::kUnsupportedOperand;
            return out;
        }
        if (insn.dst.kind != OperandKind::kRegister) {
            out.error = TranslateError::kInternal;
            return out;
        }
    }

    const a64::Width w = is32 ? a64::Width::k32 : a64::Width::k64;
    const int bits = is32 ? 32 : 64;

    e.mov_reg(a64::Width::k64, kState, 0); // x8 = GuestState*

    switch (insn.opcode) {
    case Opcode::kNop:
        emit_advance_rip(e, insn);
        break;

    case Opcode::kHlt:
        // HLT stops the program. It does not advance rip, matching the
        // interpreter's behaviour.
        emit_return(e, BlockResult::kHalted);
        break;

    case Opcode::kMov: {
        const TranslateError err = load_source(e, insn.src, kA, bits);
        if (err != TranslateError::kNone) {
            out.error = err;
            return out;
        }
        store_gpr(e, kA, insn.dst.reg);
        emit_advance_rip(e, insn);
        break;
    }

    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kCmp: {
        load_gpr(e, insn.dst.reg, kA);
        const TranslateError err = load_source(e, insn.src, kB, bits);
        if (err != TranslateError::kNone) {
            out.error = err;
            return out;
        }

        const bool is_logical = insn.opcode == Opcode::kAnd || insn.opcode == Opcode::kOr ||
                                insn.opcode == Opcode::kXor;
        const bool is_sub = insn.opcode == Opcode::kSub || insn.opcode == Opcode::kCmp;

        if (is_logical) {
            switch (insn.opcode) {
            case Opcode::kAnd:
                e.and_(w, kR, kA, kB);
                break;
            case Opcode::kOr:
                e.orr(w, kR, kA, kB);
                break;
            default:
                e.eor(w, kR, kA, kB);
                break;
            }
            emit_flags_logical(e, w, kR);
        } else {
            // t2 = a ^ b, completed to (a ^ b ^ r) after the op for AF.
            e.eor(a64::Width::k64, kT2, kA, kB);
            if (is_sub) {
                e.subs(w, kR, kA, kB);
            } else {
                e.adds(w, kR, kA, kB);
            }
            e.eor(a64::Width::k64, kT2, kT2, kR);
            emit_flags_from_nzcv(e, is_sub, kT2, kR);
        }

        if (insn.opcode != Opcode::kCmp) {
            store_gpr(e, kR, insn.dst.reg);
        }
        emit_advance_rip(e, insn);
        break;
    }

    case Opcode::kInc:
    case Opcode::kDec: {
        // INC/DEC are ADD/SUB of 1, except that CF is preserved. Save CF,
        // run the flag-producing op, then restore the saved CF.
        e.ldrb(kT0, kState, kCfOffset); // kT0 = old CF (not clobbered below)
        load_gpr(e, insn.dst.reg, kA);
        e.movz(a64::Width::k32, kB, 1);
        const bool is_sub = insn.opcode == Opcode::kDec;
        e.eor(a64::Width::k64, kT2, kA, kB);
        if (is_sub) {
            e.subs(w, kR, kA, kB);
        } else {
            e.adds(w, kR, kA, kB);
        }
        e.eor(a64::Width::k64, kT2, kT2, kR);
        emit_flags_from_nzcv(e, is_sub, kT2, kR);
        e.strb(kT0, kState, kCfOffset); // restore CF
        store_gpr(e, kR, insn.dst.reg);
        emit_advance_rip(e, insn);
        break;
    }

    case Opcode::kJmp:
    case Opcode::kJcc:
    case Opcode::kPush:
    case Opcode::kPop:
    case Opcode::kRet:
        // Control flow and the stack require block exits and a translation
        // cache; they are Phase 4. Refuse rather than miscompile.
        out.error = TranslateError::kUnsupportedOpcode;
        return out;
    }

    if (insn.opcode != Opcode::kHlt) {
        emit_return(e, BlockResult::kOk);
    }

    out.code = e.bytes();
    out.instruction_count = e.count();
    return out;
}

} // namespace hyperion
