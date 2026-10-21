// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// x86-64 -> AArch64 translation of the Phase 2 subset (see translator.hpp for
// the exact scope). The generated block has the signature
// `BlockResult block(GuestState*)` and updates the guest state in place.
//
// Design notes:
//   - Guest registers are allocated to callee-saved host registers within a
//     block (Phase 5); see the RegAlloc comment below. GuestState remains
//     authoritative at block boundaries.
//   - x86 flags are *computed*, not delegated: AArch64's ADDS/SUBS set N/Z/C/V,
//     which map onto x86 SF/ZF/CF/OF directly; AF and PF are derived with two
//     ALU identities documented below. This is what makes the translation
//     independently checkable against the interpreter rather than a re-entry
//     into it.
//   - The x8 register holds the GuestState pointer. It is caller-saved in the
//     AAPCS64, so the block needs no prologue/epilogue unless it allocates.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

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

// Guest-register allocation (Phase 5, BENCHMARKS.md §4.2.6).
//
// Within a block, the most-used guest registers are held in callee-saved host
// registers (x19..x28) rather than GuestState memory. Each is loaded once at
// block entry, kept in sync by every instruction that touches it, and spilled
// once at block exit. This lets `add rbx, rax` become a single `adds x_h,
// x_h, x_h` instead of load-load-add-store.
//
// The mapping is a pure fast path: GuestState remains the authoritative home of
// every register at block entry and exit, so the block signature and the
// authoritative-rip rule are unchanged, and a block with no allocation (or a
// guest register beyond the cap) keeps the old memory lowering. Because the
// host registers are callee-saved, they are saved/restored around the block.
constexpr std::size_t kMaxAllocated = 10; // x19..x28
constexpr a64::Reg kAllocBase = 19;

struct RegAlloc {
    std::int8_t host_of_guest[kGprCount];
    RegAlloc() {
        for (auto& h : host_of_guest) {
            h = -1;
        }
    }
    bool mapped(Gpr r) const { return host_of_guest[static_cast<unsigned>(r)] >= 0; }
    a64::Reg host(Gpr r) const {
        return static_cast<a64::Reg>(host_of_guest[static_cast<unsigned>(r)]);
    }
    // The host registers actually in use, in allocation order.
    std::vector<a64::Reg> allocated;
};

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

// Register-allocation-aware load/store. If guest register `r` is allocated to a
// host register, the value is copied to/from that register; otherwise it goes
// through GuestState as before.
void load_gpr_rb(a64::Encoder& e, const RegAlloc* ra, Gpr r, a64::Reg dst) {
    if (ra != nullptr && ra->mapped(r)) {
        e.mov_reg(a64::Width::k64, dst, ra->host(r));
    } else {
        load_gpr(e, r, dst);
    }
}

void store_gpr_rb(a64::Encoder& e, const RegAlloc* ra, a64::Reg src, Gpr r) {
    if (ra != nullptr && ra->mapped(r)) {
        e.mov_reg(a64::Width::k64, ra->host(r), src);
    } else {
        store_gpr(e, src, r);
    }
}

// The guest register held in host register `h`, or XZR (31) if none. Used to
// spill an allocated register back to its GuestState slot.
Gpr guest_of_host(const RegAlloc& ra, a64::Reg h) {
    for (unsigned r = 0; r < kGprCount; ++r) {
        if (ra.host_of_guest[r] == static_cast<std::int8_t>(h)) {
            return static_cast<Gpr>(r);
        }
    }
    return static_cast<Gpr>(31);
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
    e.mvn(a64::Width::k32, dst, dst);        // ~parity_bit (leaves NZCV untouched)
    e.ubfx(a64::Width::k32, dst, dst, 0, 1); // PF = (~parity_bit) & 1
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
    case Opcode::kTest:
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

// Guest registers an instruction reads, and those it writes. Used both to pick
// which registers to allocate and to price that allocation. Conservative: it
// names every register the instruction could read or write, so the allocation is
// a pure fast path that never changes which registers exist.
void regs_read_write(const Instruction& insn, bool reads[kGprCount], bool writes[kGprCount]) {
    switch (insn.opcode) {
    case Opcode::kMov:
        if (insn.has_src && insn.src.kind == OperandKind::kRegister) {
            reads[static_cast<unsigned>(insn.src.reg)] = true;
        }
        writes[static_cast<unsigned>(insn.dst.reg)] = true;
        break;
    case Opcode::kLea:
        if (insn.has_src && insn.src.kind == OperandKind::kMemory && !insn.src.mem.rip_relative) {
            if (insn.src.mem.has_base) {
                reads[static_cast<unsigned>(insn.src.mem.base)] = true;
            }
            if (insn.src.mem.has_index) {
                reads[static_cast<unsigned>(insn.src.mem.index)] = true;
            }
        }
        writes[static_cast<unsigned>(insn.dst.reg)] = true;
        break;
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
        reads[static_cast<unsigned>(insn.dst.reg)] = true;
        if (insn.has_src && insn.src.kind == OperandKind::kRegister) {
            reads[static_cast<unsigned>(insn.src.reg)] = true;
        }
        writes[static_cast<unsigned>(insn.dst.reg)] = true;
        break;
    case Opcode::kCmp:
    case Opcode::kTest:
        // Reads both operands, writes no register (only flags).
        reads[static_cast<unsigned>(insn.dst.reg)] = true;
        if (insn.has_src && insn.src.kind == OperandKind::kRegister) {
            reads[static_cast<unsigned>(insn.src.reg)] = true;
        }
        break;
    case Opcode::kInc:
    case Opcode::kDec:
        reads[static_cast<unsigned>(insn.dst.reg)] = true;
        writes[static_cast<unsigned>(insn.dst.reg)] = true;
        break;
    default:
        // Control flow, syscall, hlt, nop: no guest register operand.
        break;
    }
}

// Instructions an allocation of `n` registers adds to the block: the
// save/restore of the borrowed callee-saved host registers (one stp/ldp per
// pair, with an odd register stored as a pair with XZR) plus one ldr in the
// prologue and one str in the epilogue per allocated guest register.
std::size_t alloc_cost(std::size_t n) {
    return 2 * n + 2 * ((n + 1) / 2);
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
        if (mask & kFbCf) {
            e.strb(a64::kXzr, kState, kCfOffset);
        }
        if (mask & kFbOf) {
            e.strb(a64::kXzr, kState, kOfOffset);
        }
        if (mask & kFbAf) {
            e.strb(a64::kXzr, kState, kAfOffset);
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
TranslateError load_source(a64::Encoder& e, const Operand& src, a64::Reg dst, int bits,
                           const RegAlloc* ra) {
    switch (src.kind) {
    case OperandKind::kRegister:
        load_gpr_rb(e, ra, src.reg, dst);
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

void emit_return(a64::Encoder& e, BlockResult result, const RegAlloc* ra = nullptr,
                 bool set_single_run = false) {
    if (ra != nullptr) {
        // Spill every allocated guest register back to its GuestState slot.
        // Spills are individual stores: two allocated host registers are not
        // necessarily consecutive in GuestState, so a pair store would write the
        // wrong slot.
        for (a64::Reg h : ra->allocated) {
            e.str(a64::Width::k64, h, kState, gpr_offset(guest_of_host(*ra, h)));
        }
        // Restore the callee-saved host registers the block borrowed, in the
        // exact reverse of the prologue. An odd count was stored as a pair with
        // XZR, so it is restored first and its second element is discarded.
        const std::size_t npairs = ra->allocated.size() / 2;
        if (ra->allocated.size() % 2 == 1) {
            e.ldp_post(ra->allocated.back(), 31, 31, 16);
        }
        for (std::size_t i = npairs; i > 0; --i) {
            e.ldp_post(ra->allocated[2 * i - 2], ra->allocated[2 * i - 1], 31, 16);
        }
    }
    if (set_single_run) {
        e.movz(a64::Width::k64, 1, 1);
    }
    e.movz(a64::Width::k64, 0, static_cast<std::uint16_t>(result));
    e.ret();
}

// Save the callee-saved host registers a block borrows. Pairs are stored with
// a pre-index writeback; an odd count stores the last alongside XZR.
void emit_prologue(a64::Encoder& e, const RegAlloc& ra) {
    for (std::size_t i = 0; i + 1 < ra.allocated.size(); i += 2) {
        e.stp_pre(ra.allocated[i], ra.allocated[i + 1], 31, -16);
    }
    if (ra.allocated.size() % 2 == 1) {
        e.stp_pre(ra.allocated.back(), 31, 31, -16);
    }
    // Load the allocated guest registers from GuestState into their host
    // registers. This must run after the saves above, which use the stack.
    for (a64::Reg h : ra.allocated) {
        e.ldr(a64::Width::k64, h, kState, gpr_offset(guest_of_host(ra, h)));
    }
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

constexpr a64::Cond invert_cond(a64::Cond c) noexcept {
    return static_cast<a64::Cond>(static_cast<std::uint8_t>(c) ^ 1u);
}

// Map an x86-64 Jcc condition directly to an AArch64 hardware NZCV condition
// code set by an immediately preceding flag-setting arithmetic instruction
// (`ADDS` for ADD/INC; `SUBS` for SUB/CMP/DEC). Returns false when no direct
// hardware condition holds (e.g. PF, or CF read after INC/DEC, or BE/A after
// ADD where `C || Z` is not a single ARMv8 condition).
bool try_nzcv_cond(Opcode prev_op, Condition cond, a64::Cond& out) noexcept {
    if (prev_op != Opcode::kAdd && prev_op != Opcode::kSub && prev_op != Opcode::kCmp &&
        prev_op != Opcode::kInc && prev_op != Opcode::kDec && prev_op != Opcode::kAnd &&
        prev_op != Opcode::kOr && prev_op != Opcode::kXor && prev_op != Opcode::kTest) {
        return false;
    }
    const std::uint8_t req = condition_flags(cond);
    if ((flags_written(prev_op) & req) != req) {
        return false; // e.g. JB/JBE after INC/DEC where CF comes from earlier
    }
    // `SUBS` (emitted for SUB/CMP/DEC and via `CMP R, #0` for AND/OR/XOR/TEST)
    // sets `(N, Z, !C, V) == (SF, ZF, CF, OF)` across all 14 non-parity x86
    // conditions, including `C = 1 == !CF` and `V = 0 == OF` for logical ops.
    const bool is_sub = (prev_op != Opcode::kAdd && prev_op != Opcode::kInc);
    switch (cond) {
    case Condition::kO:
        out = a64::Cond::kVs;
        return true;
    case Condition::kNo:
        out = a64::Cond::kVc;
        return true;
    case Condition::kE:
        out = a64::Cond::kEq;
        return true;
    case Condition::kNe:
        out = a64::Cond::kNe;
        return true;
    case Condition::kS:
        out = a64::Cond::kMi;
        return true;
    case Condition::kNs:
        out = a64::Cond::kPl;
        return true;
    case Condition::kL:
        out = a64::Cond::kLt;
        return true;
    case Condition::kGe:
        out = a64::Cond::kGe;
        return true;
    case Condition::kLe:
        out = a64::Cond::kLe;
        return true;
    case Condition::kG:
        out = a64::Cond::kGt;
        return true;
    case Condition::kB:
        out = is_sub ? a64::Cond::kCc : a64::Cond::kCs;
        return true;
    case Condition::kAe:
        out = is_sub ? a64::Cond::kCs : a64::Cond::kCc;
        return true;
    case Condition::kBe:
        if (!is_sub) {
            return false;
        }
        out = a64::Cond::kLs;
        return true;
    case Condition::kA:
        if (!is_sub) {
            return false;
        }
        out = a64::Cond::kHi;
        return true;
    case Condition::kP:
    case Condition::kNp:
        return false;
    }
    return false;
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
                        std::uint8_t live_flags, const RegAlloc* ra = nullptr) {
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
        if (write_rip) {
            emit_set_rip(e, insn.address);
        }
        emit_return(e, BlockResult::kHalted, ra);
        return TranslateError::kNone;

    case Opcode::kSyscall:
        // A syscall ends the block: the dispatcher must run the handler (which
        // needs the Memory object, unavailable in generated code) and then
        // resume. rip is advanced here so the handler, and any resume, sees the
        // instruction after `syscall`, matching a real CPU.
        emit_set_rip(e, insn.address + insn.length);
        emit_return(e, BlockResult::kSyscall, ra);
        return TranslateError::kNone;

    case Opcode::kMov: {
        // If both operands are allocated, the move is a single host-register
        // move (the 32-bit form zero-extends, matching x86-64). If the source is
        // an immediate and the destination is allocated, materialise straight
        // into the host register. Otherwise fall back to the memory path.
        if (ra != nullptr && ra->mapped(insn.dst.reg) && insn.src.kind == OperandKind::kRegister &&
            ra->mapped(insn.src.reg)) {
            e.mov_reg(w, ra->host(insn.dst.reg), ra->host(insn.src.reg));
        } else if (ra != nullptr && ra->mapped(insn.dst.reg) &&
                   insn.src.kind == OperandKind::kImmediate) {
            const std::uint64_t value =
                (bits == 32) ? (insn.src.imm & 0xFFFFFFFFULL) : insn.src.imm;
            e.mov_imm64(ra->host(insn.dst.reg), value);
        } else {
            const TranslateError err = load_source(e, insn.src, kA, bits, ra);
            if (err != TranslateError::kNone) {
                return err;
            }
            store_gpr_rb(e, ra, kA, insn.dst.reg);
        }
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;
    }

    case Opcode::kLea: {
        const a64::Reg dst =
            (ra != nullptr && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kR;
        if (insn.src.mem.rip_relative) {
            std::uint64_t val =
                (insn.address + insn.length) + static_cast<std::uint64_t>(insn.src.mem.disp);
            if (bits == 32) {
                val &= 0xFFFFFFFFULL;
            }
            e.mov_imm64(dst, val);
        } else {
            bool have_acc = false;
            if (insn.src.mem.has_base) {
                if (ra != nullptr && ra->mapped(insn.src.mem.base)) {
                    e.mov_reg(a64::Width::k64, kT0, ra->host(insn.src.mem.base));
                } else {
                    load_gpr(e, insn.src.mem.base, kT0);
                }
                have_acc = true;
            }
            if (insn.src.mem.has_index) {
                a64::Reg idx = kT1;
                if (ra != nullptr && ra->mapped(insn.src.mem.index)) {
                    idx = ra->host(insn.src.mem.index);
                } else {
                    load_gpr(e, insn.src.mem.index, kT1);
                }
                unsigned sh = 0;
                switch (insn.src.mem.scale) {
                case 2:
                    sh = 1;
                    break;
                case 4:
                    sh = 2;
                    break;
                case 8:
                    sh = 3;
                    break;
                default:
                    sh = 0;
                    break;
                }
                e.add_shifted(a64::Width::k64, kT0, have_acc ? kT0 : a64::kXzr, idx,
                              a64::Shift::kLsl, sh);
                have_acc = true;
            }
            if (insn.src.mem.disp != 0) {
                const auto disp_u64 = static_cast<std::uint64_t>(insn.src.mem.disp);
                std::uint32_t imm12 = 0;
                bool shift12 = false;
                if (!have_acc) {
                    e.mov_imm64(kT0, disp_u64);
                    have_acc = true;
                } else if (insn.src.mem.disp > 0 && encode_imm12(disp_u64, imm12, shift12)) {
                    e.add_imm(a64::Width::k64, kT0, kT0, imm12, shift12);
                } else if (insn.src.mem.disp < 0 && encode_imm12(0ULL - disp_u64, imm12, shift12)) {
                    e.sub_imm(a64::Width::k64, kT0, kT0, imm12, shift12);
                } else {
                    e.mov_imm64(kT1, disp_u64);
                    e.add(a64::Width::k64, kT0, kT0, kT1);
                }
            }
            if (!have_acc) {
                e.movz(a64::Width::k64, dst, 0);
            } else {
                e.mov_reg(w, dst, kT0);
            }
        }
        if (dst == kR) {
            store_gpr_rb(e, ra, kR, insn.dst.reg);
        }
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
    case Opcode::kCmp:
    case Opcode::kTest: {
        // Operate directly on the allocated host registers when both operands
        // are allocated; kA/kB/kR are used only as fallbacks. This is what turns
        // `add rdx, rax` into one `adds x, x, x` instead of load/load/add/store.
        const a64::Reg A =
            (ra != nullptr && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kA;
        if (A == kA) {
            load_gpr(e, insn.dst.reg, kA);
        }
        const a64::Reg B = (ra != nullptr && insn.has_src &&
                            insn.src.kind == OperandKind::kRegister && ra->mapped(insn.src.reg))
                               ? ra->host(insn.src.reg)
                               : kB;
        // CMP and TEST do not write a destination register; their result is
        // computed solely for flags.
        const bool writes_dst = insn.opcode != Opcode::kCmp && insn.opcode != Opcode::kTest;
        const a64::Reg R =
            (ra != nullptr && writes_dst && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kR;

        const bool is_logical = insn.opcode == Opcode::kAnd || insn.opcode == Opcode::kOr ||
                                insn.opcode == Opcode::kXor || insn.opcode == Opcode::kTest;
        const bool is_sub = insn.opcode == Opcode::kSub || insn.opcode == Opcode::kCmp;

        // Only flags that are written *and* read before being overwritten are
        // worth computing. An empty mask means no flag is live, so the whole
        // flag prologue disappears.
        const std::uint8_t mask =
            static_cast<std::uint8_t>(flags_written(insn.opcode) & live_flags);

        // An immediate source that fits the AArch64 ADD/SUB immediate form is
        // folded directly into the operation instead of materialising kB first.
        // Even when AF is live, AF depends only on the low 4 bits of the two
        // operands:
        //   - ADD: AF = ((a & 0xF) + (imm & 0xF)) >= 16
        //   - SUB/CMP: AF = (a & 0xF) < (imm & 0xF)
        // Notably, when (imm & 0xF) == 0 (including all shift12 multiples of
        // 0x1000), AF is identically 0 for every guest operand value.
        std::uint32_t imm12 = 0;
        bool shift12 = false;
        const std::uint64_t imm_value =
            insn.has_src ? ((bits == 32) ? (insn.src.imm & 0xFFFFFFFFULL) : insn.src.imm) : 0ULL;
        const bool imm_folded = insn.has_src && insn.src.kind == OperandKind::kImmediate &&
                                (insn.opcode == Opcode::kAdd || insn.opcode == Opcode::kSub ||
                                 insn.opcode == Opcode::kCmp) &&
                                encode_imm12(imm_value, imm12, shift12);

        if (!imm_folded && B == kB) {
            const TranslateError err = load_source(e, insn.src, kB, bits, ra);
            if (err != TranslateError::kNone) {
                return err;
            }
        }

        if (is_logical) {
            switch (insn.opcode) {
            case Opcode::kAnd:
            case Opcode::kTest:
                e.and_(w, R, A, B);
                break;
            case Opcode::kOr:
                e.orr(w, R, A, B);
                break;
            default:
                e.eor(w, R, A, B);
                break;
            }
            if (mask != 0) {
                emit_flags_logical(e, w, mask, R);
            }
        } else if (imm_folded) {
            const auto imm_nibble = static_cast<std::uint32_t>(imm_value & 0xFULL);
            a64::Reg prev_a = A;
            if ((mask & kFbAf) != 0 && imm_nibble != 0 && R == A) {
                e.mov_reg(a64::Width::k64, kT2, A);
                prev_a = kT2;
            }
            if (is_sub) {
                e.subs_imm(w, R, A, imm12, shift12);
            } else {
                e.adds_imm(w, R, A, imm12, shift12);
            }
            if ((mask & kFbAf) != 0) {
                if (imm_nibble == 0) {
                    e.strb(a64::kXzr, kState, kAfOffset);
                } else {
                    // AF == ((a ^ imm ^ r) >> 4) & 1; EOR/MVN/UBFX preserve NZCV!
                    e.eor(a64::Width::k64, kT2, prev_a, R);
                    if ((imm_value & 0x10ULL) != 0) {
                        e.mvn(a64::Width::k32, kT2, kT2);
                    }
                    e.ubfx(a64::Width::k32, kT2, kT2, 4, 1);
                    e.strb(kT2, kState, kAfOffset);
                }
            }
            const auto non_af_mask = static_cast<std::uint8_t>(mask & ~kFbAf);
            if (non_af_mask != 0) {
                emit_flags_from_nzcv(e, is_sub, non_af_mask, kT2, R);
            }
        } else {
            if (mask & kFbAf) {
                // t2 = a ^ b, completed to (a ^ b ^ r) after the op for AF.
                e.eor(a64::Width::k64, kT2, A, B);
            }
            if (is_sub) {
                e.subs(w, R, A, B);
            } else {
                e.adds(w, R, A, B);
            }
            if (mask != 0) {
                if (mask & kFbAf) {
                    e.eor(a64::Width::k64, kT2, kT2, R);
                }
                emit_flags_from_nzcv(e, is_sub, mask, kT2, R);
            }
        }

        if (writes_dst && R == kR) {
            // The result landed in the scratch register, so it must be written
            // back. When R is the destination's own host register it is already
            // in place.
            store_gpr_rb(e, ra, kR, insn.dst.reg);
        }
        if (write_rip) {
            emit_advance_rip(e, insn);
        }
        return TranslateError::kNone;
    }

    case Opcode::kInc:
    case Opcode::kDec: {
        // INC/DEC are ADD/SUB of immediate #1 with CF preserved. Using
        // ADDS_IMM/SUBS_IMM #1 avoids materialising 1 in a register and leaves
        // hardware NZCV valid for direct fusion with a following Jcc.
        const a64::Reg A =
            (ra != nullptr && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kA;
        if (A == kA) {
            load_gpr(e, insn.dst.reg, kA);
        }
        const bool is_sub = insn.opcode == Opcode::kDec;
        const std::uint8_t mask =
            static_cast<std::uint8_t>(flags_written(insn.opcode) & live_flags);
        const a64::Reg R =
            (ra != nullptr && ra->mapped(insn.dst.reg)) ? ra->host(insn.dst.reg) : kR;
        a64::Reg prev_a = A;
        if ((mask & kFbAf) != 0 && R == A) {
            e.mov_reg(a64::Width::k64, kT2, A);
            prev_a = kT2;
        }
        if (is_sub) {
            e.subs_imm(w, R, A, 1);
        } else {
            e.adds_imm(w, R, A, 1);
        }
        if (mask != 0) {
            if (mask & kFbAf) {
                e.eor(a64::Width::k64, kT2, prev_a, R);
            }
            emit_flags_from_nzcv(e, is_sub, mask, kT2, R);
        }
        if (R == kR) {
            store_gpr_rb(e, ra, kR, insn.dst.reg);
        }
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
    case Opcode::kLea:
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kCmp:
    case Opcode::kTest:
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
    if (insn.opcode == Opcode::kLea) {
        if (insn.dst.kind != OperandKind::kRegister) {
            return TranslateError::kInternal;
        }
        if (!insn.has_src || insn.src.kind != OperandKind::kMemory) {
            return TranslateError::kUnsupportedOperand;
        }
        return TranslateError::kNone;
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

    if (insn.opcode != Opcode::kNop && insn.opcode != Opcode::kHlt &&
        insn.opcode != Opcode::kSyscall) {
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

    if (insn.opcode != Opcode::kHlt && insn.opcode != Opcode::kSyscall) {
        emit_return(e, BlockResult::kOk);
    }

    out.code = e.bytes();
    out.instruction_count = e.count();
    return out;
}

BlockTranslation translate_block(const std::uint8_t* code, std::size_t size,
                                 const Instruction& first, bool enable_self_loop) {
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

    // Pass 1.5: choose which guest registers to keep in host registers for this
    // block. Without allocation every source reference emits one `ldr` from
    // GuestState and every write emits one `str`; with allocation all of those
    // disappear from the body, at the cost of one `ldr` (prologue) and one `str`
    // (epilogue) per allocated register plus the save/restore of the borrowed
    // host registers. A register is worth a slot only when its body references
    // outnumber that overhead; see alloc_cost().
    std::size_t refs[kGprCount] = {};
    for (const Step& step : steps) {
        bool r[kGprCount] = {};
        bool w[kGprCount] = {};
        regs_read_write(step.insn, r, w);
        for (unsigned i = 0; i < kGprCount; ++i) {
            refs[i] += static_cast<std::size_t>((r[i] ? 1 : 0) + (w[i] ? 1 : 0));
        }
    }

    RegAlloc ra;
    // Take the registers with the largest saving first and stop as soon as the
    // marginal register no longer covers the step increase in overhead, so a
    // block is allocated only when it nets positive.
    std::vector<unsigned> order;
    for (unsigned i = 0; i < kGprCount; ++i) {
        if (refs[i] > 0) {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
        return refs[a] != refs[b] ? refs[a] > refs[b] : a < b;
    });
    std::size_t total_saving = 0;
    std::size_t nalloc = 0;
    while (nalloc < order.size() && nalloc < kMaxAllocated) {
        const std::size_t next = nalloc + 1;
        const std::size_t gain = refs[order[nalloc]];
        if (total_saving + gain < alloc_cost(next)) {
            break;
        }
        total_saving += gain;
        ++nalloc;
    }
    for (std::size_t i = 0; i < nalloc; ++i) {
        const a64::Reg h = static_cast<a64::Reg>(kAllocBase + i);
        ra.host_of_guest[order[i]] = static_cast<std::int8_t>(h);
        ra.allocated.push_back(h);
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

    // Detect tight single-block loops (`steps[0..m-2]` followed by `Jcc` back to
    // `first.address`) where `steps[m-2]` directly drives hardware NZCV. When
    // `enable_self_loop` is set by `TranslationCache`, the entire loop executes
    // natively inside AArch64 registers for up to `max_runs` (`x1`) iterations
    // and materialises all architectural exit flags once on the cold exit path.
    a64::Cond self_loop_taken_cond = a64::Cond::kAl;
    const bool use_self_loop = enable_self_loop && steps.size() >= 2 && exit == BlockExit::kJcc &&
                               steps.back().insn.dst.imm == first.address &&
                               try_nzcv_cond(steps[steps.size() - 2].insn.opcode,
                                             steps.back().insn.condition, self_loop_taken_cond);

    if (use_self_loop) {
        constexpr a64::Reg kLoopA = 2;
        constexpr a64::Reg kLoopB = 3;
        constexpr a64::Reg kLoopRes = 4;
        constexpr a64::Reg kLoopRem = 7;

        // `x1` holds `max_runs` (>= 1) untouched across the loop; `x7` counts down.
        e.mov_reg(a64::Width::k64, kLoopRem, 1);
        emit_prologue(e, ra);

        const a64::Label loop_top = e.new_label();
        const a64::Label exit_fallthrough = e.new_label();
        const a64::Label emit_exit_flags = e.new_label();

        e.bind(loop_top);
        for (std::size_t i = 0; i + 2 < steps.size(); ++i) {
            const TranslateError emit_err =
                emit_one(e, steps[i].insn, /*write_rip=*/false, steps[i].live_flags, &ra);
            if (emit_err != TranslateError::kNone) {
                out.error = emit_err;
                return out;
            }
        }

        // Emit `steps[m - 2]` directly into its destination register `R` while
        // keeping pre-operation operand(s) in `kLoopA` (and `kLoopB` only when
        // non-immediate) so both cold exit paths produce bit-for-bit identical
        // CF/PF/AF/ZF/SF/OF in GuestState with minimal loop instructions.
        const Instruction& cmp_or_alu = steps[steps.size() - 2].insn;
        const bool is32 = cmp_or_alu.dst.size == OperandSize::k32;
        const a64::Width w = is32 ? a64::Width::k32 : a64::Width::k64;
        const int bits = is32 ? 32 : 64;
        const bool is_logical =
            cmp_or_alu.opcode == Opcode::kAnd || cmp_or_alu.opcode == Opcode::kOr ||
            cmp_or_alu.opcode == Opcode::kXor || cmp_or_alu.opcode == Opcode::kTest;
        const bool is_sub = cmp_or_alu.opcode == Opcode::kSub ||
                            cmp_or_alu.opcode == Opcode::kCmp || cmp_or_alu.opcode == Opcode::kDec;
        const bool is_inc_dec =
            cmp_or_alu.opcode == Opcode::kInc || cmp_or_alu.opcode == Opcode::kDec;
        const bool writes_dst =
            cmp_or_alu.opcode != Opcode::kCmp && cmp_or_alu.opcode != Opcode::kTest;
        const a64::Reg R =
            (writes_dst && ra.mapped(cmp_or_alu.dst.reg)) ? ra.host(cmp_or_alu.dst.reg) : kLoopRes;

        std::uint32_t loop_imm12 = 0;
        bool loop_shift12 = false;
        const std::uint64_t loop_imm_val =
            cmp_or_alu.has_src
                ? ((bits == 32) ? (cmp_or_alu.src.imm & 0xFFFFFFFFULL) : cmp_or_alu.src.imm)
                : 0ULL;
        const bool loop_imm_folded = !is_logical && !is_inc_dec && cmp_or_alu.has_src &&
                                     cmp_or_alu.src.kind == OperandKind::kImmediate &&
                                     encode_imm12(loop_imm_val, loop_imm12, loop_shift12);

        load_gpr_rb(e, &ra, cmp_or_alu.dst.reg, kLoopA);
        if (is_logical) {
            const TranslateError err = load_source(e, cmp_or_alu.src, kLoopB, bits, &ra);
            if (err != TranslateError::kNone) {
                out.error = err;
                return out;
            }
            switch (cmp_or_alu.opcode) {
            case Opcode::kAnd:
            case Opcode::kTest:
                e.and_(w, R, kLoopA, kLoopB);
                break;
            case Opcode::kOr:
                e.orr(w, R, kLoopA, kLoopB);
                break;
            default:
                e.eor(w, R, kLoopA, kLoopB);
                break;
            }
            e.cmp_imm(w, R, 0);
        } else if (is_inc_dec) {
            if (is_sub) {
                e.subs_imm(w, R, kLoopA, 1);
            } else {
                e.adds_imm(w, R, kLoopA, 1);
            }
        } else if (loop_imm_folded) {
            if (is_sub) {
                e.subs_imm(w, R, kLoopA, loop_imm12, loop_shift12);
            } else {
                e.adds_imm(w, R, kLoopA, loop_imm12, loop_shift12);
            }
        } else {
            const TranslateError err = load_source(e, cmp_or_alu.src, kLoopB, bits, &ra);
            if (err != TranslateError::kNone) {
                out.error = err;
                return out;
            }
            if (is_sub) {
                e.subs(w, R, kLoopA, kLoopB);
            } else {
                e.adds(w, R, kLoopA, kLoopB);
            }
        }
        if (writes_dst && R == kLoopRes) {
            store_gpr(e, kLoopRes, cmp_or_alu.dst.reg);
        }

        // Hot backedge: exit immediately if Jcc condition is false; otherwise
        // decrement `x7` and jump straight back to `loop_top`.
        e.b_cond(invert_cond(self_loop_taken_cond), exit_fallthrough);
        e.subs_imm(a64::Width::k64, kLoopRem, kLoopRem, 1);
        e.b_cond(a64::Cond::kNe, loop_top);

        // Cold exit A: step budget (`x1 == max_runs`) exhausted on taken backedge.
        if (is_logical) {
            e.cmp_imm(w, R, 0);
        } else if (is_inc_dec) {
            if (is_sub) {
                e.subs_imm(w, a64::kXzr, kLoopA, 1);
            } else {
                e.adds_imm(w, a64::kXzr, kLoopA, 1);
            }
        } else if (loop_imm_folded) {
            if (is_sub) {
                e.subs_imm(w, a64::kXzr, kLoopA, loop_imm12, loop_shift12);
            } else {
                e.adds_imm(w, a64::kXzr, kLoopA, loop_imm12, loop_shift12);
            }
        } else {
            if (is_sub) {
                e.subs(w, a64::kXzr, kLoopA, kLoopB);
            } else {
                e.adds(w, a64::kXzr, kLoopA, kLoopB);
            }
        }
        e.mov_imm64(kT0, steps.back().insn.dst.imm);
        e.b(emit_exit_flags);

        // Cold exit B: loop terminated naturally (`Jcc` not taken -> fallthrough).
        // Hardware NZCV is still untouched from `subs/adds R, ...` above!
        e.bind(exit_fallthrough);
        e.sub(a64::Width::k64, 1, 1, kLoopRem);
        e.add_imm(a64::Width::k64, 1, 1, 1); // x1 = runs = max_runs - rem + 1
        e.mov_imm64(kT0, steps.back().insn.address + steps.back().insn.length);

        // Shared cold epilogue: write `rip`, materialise exit flags into
        // GuestState, spill allocated GPRs, and return `(x0, x1)`.
        e.bind(emit_exit_flags);
        e.str(a64::Width::k64, kT0, kState, kRipOffset);
        const std::uint8_t exit_mask = flags_written(cmp_or_alu.opcode);
        const std::uint64_t af_imm = is_inc_dec ? 1ULL : loop_imm_val;
        if (is_logical) {
            emit_flags_logical(e, w, exit_mask, R);
        } else if (is_inc_dec || loop_imm_folded) {
            if ((exit_mask & kFbAf) != 0) {
                if ((af_imm & 0xFULL) == 0) {
                    e.strb(a64::kXzr, kState, kAfOffset);
                } else {
                    e.eor(a64::Width::k64, kT2, kLoopA, R);
                    if ((af_imm & 0x10ULL) != 0) {
                        e.mvn(a64::Width::k32, kT2, kT2);
                    }
                    e.ubfx(a64::Width::k32, kT2, kT2, 4, 1);
                    e.strb(kT2, kState, kAfOffset);
                }
            }
            emit_flags_from_nzcv(e, is_sub, static_cast<std::uint8_t>(exit_mask & ~kFbAf), kT2, R);
        } else {
            if ((exit_mask & kFbAf) != 0) {
                e.eor(a64::Width::k64, kT2, kLoopA, kLoopB);
                e.eor(a64::Width::k64, kT2, kT2, R);
            }
            emit_flags_from_nzcv(e, is_sub, exit_mask, kT2, R);
        }
        emit_return(e, BlockResult::kOk, &ra, /*set_single_run=*/false);

        out.exit = exit;
        out.target = target;
        out.guest_instruction_count = steps.size();
        out.code = e.bytes();
        out.instruction_count = e.count();
        return out;
    }

    // Standard single-execution block emission (with direct hardware NZCV fusion
    // on `ALU/CMP/INC/DEC -> Jcc`).
    emit_prologue(e, ra);

    for (std::size_t idx = 0; idx < steps.size(); ++idx) {
        const Step& step = steps[idx];
        const Opcode op = step.insn.opcode;
        if (op == Opcode::kHlt || op == Opcode::kRet) {
            emit_set_rip(e, step.insn.address);
            emit_return(e, BlockResult::kHalted, &ra, enable_self_loop);
            break;
        }
        if (op == Opcode::kSyscall) {
            emit_set_rip(e, step.insn.address + step.insn.length);
            emit_return(e, BlockResult::kSyscall, &ra, enable_self_loop);
            break;
        }
        if (op == Opcode::kJmp) {
            emit_set_rip(e, step.insn.dst.imm);
            emit_return(e, BlockResult::kOk, &ra, enable_self_loop);
            break;
        }
        if (op == Opcode::kJcc) {
            const std::uint64_t fallthrough = step.insn.address + step.insn.length;
            const a64::Label taken = e.new_label();
            const a64::Label done = e.new_label();
            a64::Cond fused = a64::Cond::kAl;
            if (idx > 0 && try_nzcv_cond(steps[idx - 1].insn.opcode, step.insn.condition, fused)) {
                e.b_cond(fused, taken);
            } else {
                emit_cond_branch(e, step.insn.condition, taken);
            }
            e.mov_imm64(kT0, fallthrough);
            e.b(done);
            e.bind(taken);
            e.mov_imm64(kT0, step.insn.dst.imm);
            e.bind(done);
            e.str(a64::Width::k64, kT0, kState, kRipOffset);
            emit_return(e, BlockResult::kOk, &ra, enable_self_loop);
            break;
        }
        const TranslateError emit_err =
            emit_one(e, step.insn, /*write_rip=*/false, step.live_flags, &ra);
        if (emit_err != TranslateError::kNone) {
            out.error = emit_err;
            return out;
        }
    }

    if (exit == BlockExit::kFallThrough) {
        emit_set_rip(e, target);
        emit_return(e, BlockResult::kOk, &ra, enable_self_loop);
    }

    out.exit = exit;
    out.target = target;
    out.guest_instruction_count = steps.size();
    out.code = e.bytes();
    out.instruction_count = e.count();
    return out;
}

} // namespace hyperion
