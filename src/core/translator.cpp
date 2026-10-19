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

// Emit `rip = value` for a constant guest address.
void emit_set_rip(a64::Encoder& e, std::uint64_t value) {
    e.mov_imm64(kT0, value);
    e.str(a64::Width::k64, kT0, kState, kRipOffset);
}

// Emit `rip = instruction.address + instruction.length`.
void emit_advance_rip(a64::Encoder& e, const Instruction& insn) {
    emit_set_rip(e, insn.address + insn.length);
}

// Branch to `taken` if the x86 condition `c` holds for the modelled flags in
// GuestState, otherwise fall through.
//
// x86 flags live in memory, not in AArch64's NZCV, and several x86 conditions
// (PF; the SF/OF combinations L/GE/LE/G) have no single AArch64 equivalent. So
// rather than map conditions onto NZCV, each condition loads exactly the flag
// bytes it reads, materialises a boolean, and branches on zero/non-zero. This
// is a few more instructions than a clever mapping but is straightforward to
// check against alu::evaluate(), which is the oracle.
void emit_cond_branch(a64::Encoder& e, Condition c, a64::Label taken) {
    auto load = [&](a64::Reg dst, unsigned off) { e.ldrb(dst, kState, off); };
    auto branch_if = [&](a64::Reg src, bool want_nonzero) {
        e.cmp_imm(a64::Width::k32, src, 0);
        e.b_cond(want_nonzero ? a64::Cond::kNe : a64::Cond::kEq, taken);
    };

    switch (c) {
    case Condition::kO:
    case Condition::kNo:
        load(kT0, kOfOffset);
        branch_if(kT0, c == Condition::kO);
        return;
    case Condition::kB:
    case Condition::kAe:
        load(kT0, kCfOffset);
        branch_if(kT0, c == Condition::kB);
        return;
    case Condition::kE:
    case Condition::kNe:
        load(kT0, kZfOffset);
        branch_if(kT0, c == Condition::kE);
        return;
    case Condition::kS:
    case Condition::kNs:
        load(kT0, kSfOffset);
        branch_if(kT0, c == Condition::kS);
        return;
    case Condition::kP:
    case Condition::kNp:
        load(kT0, kPfOffset);
        branch_if(kT0, c == Condition::kP);
        return;
    case Condition::kBe:
    case Condition::kA: {
        load(kT0, kCfOffset);
        load(kT1, kZfOffset);
        e.orr(a64::Width::k32, kT2, kT0, kT1); // kT2 = CF | ZF
        branch_if(kT2, c == Condition::kBe);
        return;
    }
    case Condition::kL:
    case Condition::kGe: {
        load(kT0, kSfOffset);
        load(kT1, kOfOffset);
        e.eor(a64::Width::k32, kT2, kT0, kT1); // kT2 = SF ^ OF
        branch_if(kT2, c == Condition::kL);
        return;
    }
    case Condition::kLe:
    case Condition::kG: {
        load(kT0, kSfOffset);
        load(kT1, kOfOffset);
        load(kT2, kZfOffset);
        e.eor(a64::Width::k32, kT3, kT0, kT1); // kT3 = SF ^ OF
        e.orr(a64::Width::k32, kT3, kT3, kT2); // kT3 = (SF ^ OF) | ZF
        branch_if(kT3, c == Condition::kLe);
        return;
    }
    }
}

} // namespace

const char* to_string(BlockExit exit) {
    switch (exit) {
    case BlockExit::kFallThrough:
        return "fall-through";
    case BlockExit::kJmp:
        return "jmp";
    case BlockExit::kJcc:
        return "jcc";
    case BlockExit::kHalted:
        return "halted";
    }
    return "?";
}

// Emit the AArch64 for one already-validated instruction into `e`. `e` must
// already have x8 = GuestState*. Returns a translate error, or kNone on
// success. Control-flow instructions are handled by the block translator, not
// here, so they are rejected as unsupported by this helper.
namespace {

TranslateError emit_one(a64::Encoder& e, const Instruction& insn) {
    const bool is32 = insn.dst.size == OperandSize::k32;
    const a64::Width w = is32 ? a64::Width::k32 : a64::Width::k64;
    const int bits = is32 ? 32 : 64;

    switch (insn.opcode) {
    case Opcode::kNop:
        emit_advance_rip(e, insn);
        return TranslateError::kNone;

    case Opcode::kHlt:
        emit_return(e, BlockResult::kHalted);
        return TranslateError::kNone;

    case Opcode::kMov: {
        const TranslateError err = load_source(e, insn.src, kA, bits);
        if (err != TranslateError::kNone) {
            return err;
        }
        store_gpr(e, kA, insn.dst.reg);
        emit_advance_rip(e, insn);
        return TranslateError::kNone;
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
            return err;
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
        return TranslateError::kNone;
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
        return TranslateError::kNone;
    }

    case Opcode::kJmp:
    case Opcode::kJcc:
    case Opcode::kPush:
    case Opcode::kPop:
    case Opcode::kRet:
        return TranslateError::kUnsupportedOpcode;
    }
    return TranslateError::kInternal;
}

// Whether the block translator may include this instruction. Control flow is
// handled by the block driver; PUSH/POP need a guest stack (Phase 6) and are
// cut, so the dispatcher stops at them rather than miscompiling.
bool is_block_body_opcode(Opcode op) {
    switch (op) {
    case Opcode::kMov:
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kCmp:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kInc:
    case Opcode::kDec:
    case Opcode::kNop:
        return true;
    default:
        return false;
    }
}

// Validate the operand shape of a body instruction, matching translate_instruction.
TranslateError validate_body(const Instruction& insn) {
    if (insn.dst.size != OperandSize::k32 && insn.dst.size != OperandSize::k64) {
        return TranslateError::kUnsupportedSize;
    }
    if (insn.dst.kind == OperandKind::kMemory) {
        return TranslateError::kUnsupportedOperand;
    }
    if (insn.dst.kind != OperandKind::kRegister) {
        return TranslateError::kInternal;
    }
    if (insn.has_src && insn.src.kind == OperandKind::kMemory) {
        return TranslateError::kUnsupportedOperand;
    }
    return TranslateError::kNone;
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
    e.mov_reg(a64::Width::k64, kState, 0); // x8 = GuestState*

    // Control flow is not meaningful for a lone instruction: a standalone block
    // cannot branch anywhere. Refuse it here; translate_block() handles it.
    if (insn.opcode == Opcode::kJmp || insn.opcode == Opcode::kJcc || insn.opcode == Opcode::kRet ||
        insn.opcode == Opcode::kPush || insn.opcode == Opcode::kPop) {
        out.error = TranslateError::kUnsupportedOpcode;
        return out;
    }

    if (insn.opcode != Opcode::kNop && insn.opcode != Opcode::kHlt) {
        const TranslateError err = validate_body(insn);
        if (err != TranslateError::kNone) {
            out.error = err;
            return out;
        }
    }

    const TranslateError err = emit_one(e, insn);
    if (err != TranslateError::kNone) {
        out.error = err;
        return out;
    }

    if (insn.opcode != Opcode::kHlt) {
        emit_return(e, BlockResult::kOk);
    }

    out.code = e.bytes();
    out.instruction_count = e.count();
    return out;
}

BlockTranslation translate_block(const std::uint8_t* code, std::size_t size,
                                 const Instruction& first) {
    BlockTranslation out;
    a64::Encoder e;
    e.mov_reg(a64::Width::k64, kState, 0); // x8 = GuestState*

    // `code`/`size` start at `first.address`, so an instruction at guest address
    // A sits at byte offset A - first.address.
    Instruction insn = first;
    std::size_t next_offset = first.length;

    while (true) {
        const Opcode op = insn.opcode;

        if (op == Opcode::kHlt || op == Opcode::kRet) {
            // Both stop the program. RET is modelled by the interpreter as a
            // top-level return (there is no guest stack yet, Phase 6).
            emit_return(e, BlockResult::kHalted);
            out.exit = BlockExit::kHalted;
            ++out.guest_instruction_count;
            break;
        }
        if (op == Opcode::kJmp) {
            emit_set_rip(e, insn.dst.imm);
            emit_return(e, BlockResult::kOk);
            out.exit = BlockExit::kJmp;
            out.target = insn.dst.imm;
            ++out.guest_instruction_count;
            break;
        }
        if (op == Opcode::kJcc) {
            // Evaluate the x86 condition and write the correct next rip on both
            // paths, so the dispatcher only has to read rip back.
            const std::uint64_t fallthrough = insn.address + insn.length;
            const a64::Label taken = e.new_label();
            const a64::Label done = e.new_label();
            emit_cond_branch(e, insn.condition, taken);
            emit_set_rip(e, fallthrough);
            e.b(done);
            e.bind(taken);
            emit_set_rip(e, insn.dst.imm);
            e.bind(done);
            emit_return(e, BlockResult::kOk);
            out.exit = BlockExit::kJcc;
            out.target = fallthrough; // hint only; rip is authoritative
            ++out.guest_instruction_count;
            break;
        }

        // Not a terminator. If we can translate it, emit it and continue.
        // Otherwise cut the block *before* it: the dispatcher then sees the
        // unsupported PC on the next lookup and stops, instead of running an
        // instruction we did not translate.
        //
        // The cut is only valid once the block covers at least one instruction.
        // If the *first* instruction is unsupported there is nothing to cut
        // back to, and an empty block would fall through to the same PC and loop
        // forever, so we report the error and let the caller stop.
        if (!is_block_body_opcode(op) || validate_body(insn) != TranslateError::kNone) {
            if (out.guest_instruction_count == 0) {
                out.error = TranslateError::kUnsupportedOpcode;
                return out;
            }
            out.exit = BlockExit::kFallThrough;
            out.target = insn.address;
            break;
        }
        const TranslateError emit_err = emit_one(e, insn);
        if (emit_err != TranslateError::kNone) {
            if (out.guest_instruction_count == 0) {
                out.error = emit_err;
                return out;
            }
            out.exit = BlockExit::kFallThrough;
            out.target = insn.address;
            break;
        }
        ++out.guest_instruction_count;

        if (next_offset >= size) {
            out.exit = BlockExit::kFallThrough;
            out.target = first.address + next_offset;
            break;
        }
        const DecodeResult next =
            decode(code + next_offset, size - next_offset, first.address + next_offset);
        if (!next.ok()) {
            out.exit = BlockExit::kFallThrough;
            out.target = first.address + next_offset;
            break;
        }
        insn = next.instruction;
        next_offset += insn.length;
    }

    if (out.exit == BlockExit::kFallThrough) {
        emit_return(e, BlockResult::kOk);
    }

    out.code = e.bytes();
    out.instruction_count = e.count();
    return out;
}

} // namespace hyperion
