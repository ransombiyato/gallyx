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
// the eight bits gives the odd/even bit; inverting it gives PF. Three shifted
// XORs fold 8 -> 4 -> 2 -> 1 bits, then a UBFX isolates the bit and an XOR with
// 1 inverts it. This replaces a 16-instruction per-bit fold plus its setup (19
// instructions in total) with 6.
void emit_parity(a64::Encoder& e, a64::Reg dst, a64::Reg src) {
    e.eor_shifted(a64::Width::k32, dst, src, src, a64::Shift::kLsr, 4); // b7..b4 ^ b3..b0
    e.eor_shifted(a64::Width::k32, dst, dst, dst, a64::Shift::kLsr, 2);
    e.eor_shifted(a64::Width::k32, dst, dst, dst, a64::Shift::kLsr, 1);
    e.ubfx(a64::Width::k32, dst, dst, 0, 1); // low bit = XOR of the eight bits
    e.movz(a64::Width::k32, kT3, 1);
    e.eor(a64::Width::k32, dst, dst, kT3); // PF = 1 - parity_bit
}

// --- Flag liveness ----------------------------------------------------------
//
// Which x86 flags an instruction reads and writes. Modelling this lets the
// translator skip computing a flag whose value is overwritten before anything
// reads it, which is most of the emitted code for an ALU instruction.
//
// The subtlety that a naive "every ALU op writes all six flags" model gets
// wrong is INC/DEC: they are ADD/SUB of 1 *with CF held*, so they write
// PF/AF/ZF/SF/OF but leave CF unchanged. flags_written() encodes that, and the
// emission never stores CF for INC/DEC, so CF is preserved for free.
enum FlagBit : std::uint8_t {
    kFbCf = 1u << 0,
    kFbPf = 1u << 1,
    kFbAf = 1u << 2,
    kFbZf = 1u << 3,
    kFbSf = 1u << 4,
    kFbOf = 1u << 5,
};
constexpr std::uint8_t kFbAll = 0x3F;

// Flags an instruction writes. Everything outside the flag-setting subset
// (MOV, NOP, JMP, HLT, ...) writes none.
std::uint8_t flags_written(Opcode op) {
    switch (op) {
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kCmp:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
        return kFbAll;
    case Opcode::kInc:
    case Opcode::kDec:
        return static_cast<std::uint8_t>(kFbAll & ~kFbCf);
    default:
        return 0;
    }
}

// Flags a condition reads. Must agree with alu::evaluate()/Condition, which the
// differential tests check directly.
std::uint8_t condition_flags(Condition c) {
    switch (c) {
    case Condition::kO:
    case Condition::kNo:
        return kFbOf;
    case Condition::kB:
    case Condition::kAe:
        return kFbCf;
    case Condition::kE:
    case Condition::kNe:
        return kFbZf;
    case Condition::kS:
    case Condition::kNs:
        return kFbSf;
    case Condition::kP:
    case Condition::kNp:
        return kFbPf;
    case Condition::kBe:
    case Condition::kA:
        return static_cast<std::uint8_t>(kFbCf | kFbZf);
    case Condition::kL:
    case Condition::kGe:
        return static_cast<std::uint8_t>(kFbSf | kFbOf);
    case Condition::kLe:
    case Condition::kG:
        return static_cast<std::uint8_t>(kFbSf | kFbOf | kFbZf);
    }
    return 0;
}

// Store the masked flags from the AArch64 condition flags, which must have been
// set by a preceding ADDS (is_sub=false) or SUBS (is_sub=true). `t2` must hold
// (a ^ b ^ result) and is read only when AF is requested; `result` is used for
// parity. Flags outside `mask` are not written, so their previous value stands.
void emit_flags_from_nzcv(a64::Encoder& e, bool is_sub, std::uint8_t mask, a64::Reg t2,
                          a64::Reg result) {
    if (mask & kFbZf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kEq); // ZF = Z
        e.strb(kT1, kState, kZfOffset);
    }
    if (mask & kFbSf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kMi); // SF = N
        e.strb(kT1, kState, kSfOffset);
    }
    if (mask & kFbCf) {
        // x86 CF is the carry-out: for add it is "carry set" (CS); for subtract
        // it is "borrow" (CC, i.e. no borrow), the standard ADDS/SUBS mapping.
        e.cset(a64::Width::k32, kT1, is_sub ? a64::Cond::kCc : a64::Cond::kCs);
        e.strb(kT1, kState, kCfOffset);
    }
    if (mask & kFbOf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kVs); // OF = V
        e.strb(kT1, kState, kOfOffset);
    }
    if (mask & kFbAf) {
        // AF: carry out of bit 3. For both ADD and SUB, AF == bit 4 of
        // (a ^ b ^ r).
        e.ubfx(a64::Width::k32, kT1, t2, 4, 1);
        e.strb(kT1, kState, kAfOffset);
    }
    if (mask & kFbPf) {
        // Parity is computed last: it clobbers the condition flags, which the
        // csets above need.
        emit_parity(e, kT1, result);
        e.strb(kT1, kState, kPfOffset);
    }
}

// Logical ops (AND/OR/XOR) clear CF, OF and AF, and set SF/ZF/PF from the
// result. There is no NZCV involvement.
void emit_flags_logical(a64::Encoder& e, a64::Width w, std::uint8_t mask, a64::Reg result) {
    if (mask & (kFbZf | kFbSf)) {
        e.cmp_imm(w, result, 0); // sets N (sign) and Z from the result
    }
    if (mask & kFbZf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kEq);
        e.strb(kT1, kState, kZfOffset);
    }
    if (mask & kFbSf) {
        e.cset(a64::Width::k32, kT1, a64::Cond::kMi);
        e.strb(kT1, kState, kSfOffset);
    }
    if (mask & (kFbCf | kFbOf | kFbAf)) {
        e.movz(a64::Width::k32, kT1, 0);
        if (mask & kFbCf) {
            e.strb(kT1, kState, kCfOffset);
        }
        if (mask & kFbOf) {
            e.strb(kT1, kState, kOfOffset);
        }
        if (mask & kFbAf) {
            e.strb(kT1, kState, kAfOffset);
        }
    }
    if (mask & kFbPf) {
        emit_parity(e, kT1, result);
        e.strb(kT1, kState, kPfOffset);
    }
}

// Whether `value` fits the AArch64 ADD/SUB 12-bit immediate form (imm12,
// optionally shifted left by 12). If so, fills `imm12`/`shift12`.
bool encode_imm12(std::uint64_t value, std::uint32_t& imm12, bool& shift12) {
    if (value <= 0xFFFu) {
        imm12 = static_cast<std::uint32_t>(value);
        shift12 = false;
        return true;
    }
    if ((value & 0xFFFu) == 0 && (value >> 12) <= 0xFFFu) {
        imm12 = static_cast<std::uint32_t>(value >> 12);
        shift12 = true;
        return true;
    }
    return false;
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
    case BlockExit::kSyscall:
        return "syscall";
    }
    return "?";
}

// Emit the AArch64 for one already-validated instruction into `e`. `e` must
// already have x8 = GuestState*. Returns a translate error, or kNone on
// success. Control-flow instructions are handled by the block translator, not
// here, so they are rejected as unsupported by this helper.
namespace {

TranslateError emit_one(a64::Encoder& e, const Instruction& insn, bool write_rip,
                        std::uint8_t live_flags) {
    const bool is32 = insn.dst.size == OperandSize::k32;
    const a64::Width w = is32 ? a64::Width::k32 : a64::Width::k64;
    const int bits = is32 ? 32 : 64;

    switch (insn.opcode) {
    case Opcode::kNop:
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;

    case Opcode::kHlt:
        emit_return(e, BlockResult::kHalted);
        return TranslateError::kNone;

    case Opcode::kSyscall:
        // A syscall ends the block: the dispatcher must run the handler (which
        // needs the Memory object, unavailable in generated code) and then
        // resume. rip is advanced here so the handler, and any resume, sees the
        // instruction after `syscall`, matching a real CPU.
        emit_set_rip(e, insn.address + insn.length);
        emit_return(e, BlockResult::kSyscall);
        return TranslateError::kNone;

    case Opcode::kMov: {
        const TranslateError err = load_source(e, insn.src, kA, bits);
        if (err != TranslateError::kNone) {
            return err;
        }
        store_gpr(e, kA, insn.dst.reg);
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;
    }

    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kCmp: {
        load_gpr(e, insn.dst.reg, kA);

        const bool is_logical = insn.opcode == Opcode::kAnd || insn.opcode == Opcode::kOr ||
                                insn.opcode == Opcode::kXor;
        const bool is_sub = insn.opcode == Opcode::kSub || insn.opcode == Opcode::kCmp;

        // Only flags that are written *and* read before being overwritten are
        // worth computing. An empty mask means no flag is live, so the whole
        // flag prologue disappears.
        const std::uint8_t mask =
            static_cast<std::uint8_t>(flags_written(insn.opcode) & live_flags);

        // An immediate source that fits the AArch64 ADD/SUB immediate form is
        // folded into the operation instead of being materialised into kB first
        // (mov_imm64 is 1-4 instructions) - but only when AF is not live. AF is
        // derived from (a ^ b ^ r), which needs b in a register, so when AF is
        // needed we keep the register form and stay bit-exact rather than
        // trading correctness for size.
        std::uint32_t imm12 = 0;
        bool shift12 = false;
        const bool imm_foldable =
            insn.has_src && insn.src.kind == OperandKind::kImmediate &&
            (insn.opcode == Opcode::kAdd || insn.opcode == Opcode::kSub ||
             insn.opcode == Opcode::kCmp) &&
            encode_imm12((bits == 32) ? (insn.src.imm & 0xFFFFFFFFULL) : insn.src.imm, imm12,
                         shift12);
        const bool imm_folded = imm_foldable && (mask & kFbAf) == 0;

        if (!imm_folded) {
            const TranslateError err = load_source(e, insn.src, kB, bits);
            if (err != TranslateError::kNone) {
                return err;
            }
        }

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
            if (mask != 0) {
                emit_flags_logical(e, w, mask, kR);
            }
        } else {
            if (mask & kFbAf) {
                // t2 = a ^ b, completed to (a ^ b ^ r) after the op for AF.
                e.eor(a64::Width::k64, kT2, kA, kB);
            }
            if (imm_folded) {
                if (is_sub) {
                    e.subs_imm(w, kR, kA, imm12);
                } else {
                    e.adds_imm(w, kR, kA, imm12);
                }
            } else if (is_sub) {
                e.subs(w, kR, kA, kB);
            } else {
                e.adds(w, kR, kA, kB);
            }
            if (mask != 0) {
                if (mask & kFbAf) {
                    e.eor(a64::Width::k64, kT2, kT2, kR);
                }
                emit_flags_from_nzcv(e, is_sub, mask, kT2, kR);
            }
        }

        if (insn.opcode != Opcode::kCmp) {
            store_gpr(e, kR, insn.dst.reg);
        }
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;
    }

    case Opcode::kInc:
    case Opcode::kDec: {
        // INC/DEC are ADD/SUB of 1, except that CF is preserved. flags_written()
        // excludes CF for these, so CF is never stored and its old value stands;
        // no explicit save/restore is needed.
        load_gpr(e, insn.dst.reg, kA);
        e.movz(a64::Width::k32, kB, 1);
        const bool is_sub = insn.opcode == Opcode::kDec;
        const std::uint8_t mask =
            static_cast<std::uint8_t>(flags_written(insn.opcode) & live_flags);
        if (mask & kFbAf) {
            e.eor(a64::Width::k64, kT2, kA, kB);
        }
        if (is_sub) {
            e.subs(w, kR, kA, kB);
        } else {
            e.adds(w, kR, kA, kB);
        }
        if (mask != 0) {
            if (mask & kFbAf) {
                e.eor(a64::Width::k64, kT2, kT2, kR);
            }
            emit_flags_from_nzcv(e, is_sub, mask, kT2, kR);
        }
        store_gpr(e, kR, insn.dst.reg);
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
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

    const TranslateError err = emit_one(e, insn, /*write_rip=*/true, kFbAll);
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

    // Pass 1: decode the block into a straight-line list of instructions. This
    // has to happen before emission so flag liveness can be computed backward
    // over the whole block (pass 2); the emitter (pass 3) then knows, for each
    // instruction, which flags are actually read before being overwritten.
    struct Step {
        Instruction insn;
        std::uint8_t live_flags = 0;
    };
    std::vector<Step> steps;

    // `code`/`size` start at `first.address`, so an instruction at guest address
    // A sits at byte offset A - first.address.
    Instruction insn = first;
    std::size_t next_offset = first.length;
    BlockExit exit = BlockExit::kFallThrough;
    std::uint64_t target = 0;

    while (true) {
        const Opcode op = insn.opcode;

        if (op == Opcode::kHlt || op == Opcode::kRet) {
            // Both stop the program. RET is modelled by the interpreter as a
            // top-level return (there is no guest stack yet, Phase 6).
            steps.push_back({insn});
            exit = BlockExit::kHalted;
            break;
        }
        if (op == Opcode::kSyscall) {
            // End the block at the syscall so the dispatcher can run the
            // handler. rip is advanced past the instruction, so a resume (or the
            // next block lookup) continues correctly.
            steps.push_back({insn});
            exit = BlockExit::kSyscall;
            target = insn.address + insn.length;
            break;
        }
        if (op == Opcode::kJmp) {
            steps.push_back({insn});
            exit = BlockExit::kJmp;
            target = insn.dst.imm;
            break;
        }
        if (op == Opcode::kJcc) {
            steps.push_back({insn});
            exit = BlockExit::kJcc;
            target = insn.address + insn.length; // hint only; rip is authoritative
            break;
        }

        // Not a terminator. If we can translate it, add it and continue.
        // Otherwise cut the block *before* it: the dispatcher then sees the
        // unsupported PC on the next lookup and stops, instead of running an
        // instruction we did not translate.
        //
        // The cut is only valid once the block covers at least one instruction.
        // If the *first* instruction is unsupported there is nothing to cut
        // back to, and an empty block would fall through to the same PC and loop
        // forever, so we report the error and let the caller stop.
        if (!is_block_body_opcode(op) || validate_body(insn) != TranslateError::kNone) {
            if (steps.empty()) {
                out.error = TranslateError::kUnsupportedOpcode;
                return out;
            }
            exit = BlockExit::kFallThrough;
            target = insn.address;
            break;
        }
        steps.push_back({insn});

        if (next_offset >= size) {
            exit = BlockExit::kFallThrough;
            target = first.address + next_offset;
            break;
        }
        const DecodeResult next =
            decode(code + next_offset, size - next_offset, first.address + next_offset);
        if (!next.ok()) {
            exit = BlockExit::kFallThrough;
            target = first.address + next_offset;
            break;
        }
        insn = next.instruction;
        next_offset += insn.length;
    }

    // Pass 2: backward flag liveness. Flags live in GuestState and persist
    // across blocks, so at a block exit every flag is conservatively live: a
    // successor block may read any of them, and there is no cross-block
    // analysis. (This is sound because the invariant "all flags are correct at
    // block entry" then holds by induction from program start, where both tiers
    // begin with the same zeroed flags.)
    //
    // live_out = flags that must be correct just after this instruction.
    // live_in  = reads | (live_out & ~writes) = flags that must be correct on
    //            entry. A flag the instruction writes is *computed* only if it
    //            is also live_out; a flag it does not write is merely preserved
    //            (never stored), which is exactly how INC/DEC keep CF.
    std::uint8_t live_out = kFbAll;
    for (std::size_t i = steps.size(); i-- > 0;) {
        const Instruction& in = steps[i].insn;
        const std::uint8_t reads =
            (in.opcode == Opcode::kJcc) ? condition_flags(in.condition) : std::uint8_t{0};
        const std::uint8_t writes = flags_written(in.opcode);
        steps[i].live_flags = static_cast<std::uint8_t>(writes & live_out);
        live_out = static_cast<std::uint8_t>(reads | (live_out & ~writes));
    }

    // Pass 3: emit. Body instructions do not write rip: nothing inside a block
    // reads it, so the per-instruction store is dead. The block writes rip once,
    // at its exit. See BENCHMARKS.md §4.2.2.
    a64::Encoder e;
    e.mov_reg(a64::Width::k64, kState, 0); // x8 = GuestState*

    for (const Step& step : steps) {
        const Opcode op = step.insn.opcode;
        if (op == Opcode::kHlt || op == Opcode::kRet) {
            emit_return(e, BlockResult::kHalted);
            break;
        }
        if (op == Opcode::kSyscall) {
            emit_set_rip(e, step.insn.address + step.insn.length);
            emit_return(e, BlockResult::kSyscall);
            break;
        }
        if (op == Opcode::kJmp) {
            emit_set_rip(e, step.insn.dst.imm);
            emit_return(e, BlockResult::kOk);
            break;
        }
        if (op == Opcode::kJcc) {
            // Evaluate the x86 condition and write the correct next rip on both
            // paths, so the dispatcher only has to read rip back.
            const std::uint64_t fallthrough = step.insn.address + step.insn.length;
            const a64::Label taken = e.new_label();
            const a64::Label done = e.new_label();
            emit_cond_branch(e, step.insn.condition, taken);
            emit_set_rip(e, fallthrough);
            e.b(done);
            e.bind(taken);
            emit_set_rip(e, step.insn.dst.imm);
            e.bind(done);
            emit_return(e, BlockResult::kOk);
            break;
        }
        const TranslateError emit_err =
            emit_one(e, step.insn, /*write_rip=*/false, step.live_flags);
        if (emit_err != TranslateError::kNone) {
            // validate_body() already rejected every operand shape emit_one()
            // cannot handle, so reaching here is an internal inconsistency.
            out.error = emit_err;
            return out;
        }
    }

    if (exit == BlockExit::kFallThrough) {
        emit_set_rip(e, target);
        emit_return(e, BlockResult::kOk);
    }

    out.exit = exit;
    out.target = target;
    out.guest_instruction_count = steps.size();
    out.code = e.bytes();
    out.instruction_count = e.count();
    return out;
}

} // namespace hyperion
