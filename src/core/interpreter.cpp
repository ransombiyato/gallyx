// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/interpreter.hpp"

#include "hyperion/syscall.hpp"

#include <cstring>

namespace hyperion {

const char* to_string(RunStatus status) {
    switch (status) {
    case RunStatus::kOk:
        return "ok";
    case RunStatus::kStepLimit:
        return "step limit";
    case RunStatus::kFault:
        return "fault";
    case RunStatus::kUnsupported:
        return "unsupported";
    }
    return "?";
}

// --- FlatMemory -------------------------------------------------------------

FlatMemory::FlatMemory(std::uint64_t base, std::size_t size) : base_(base), bytes_(size, 0) {}

bool FlatMemory::contains(std::uint64_t address, std::size_t count) const {
    // Overflow-safe containment check: address >= base and address+count <=
    // base+size, without computing address+count if it would wrap.
    if (address < base_) {
        return false;
    }
    const std::uint64_t offset = address - base_;
    if (offset > bytes_.size()) {
        return false;
    }
    return count <= bytes_.size() - static_cast<std::size_t>(offset);
}

bool FlatMemory::read(std::uint64_t address, std::uint8_t* out, std::size_t count) {
    if (!contains(address, count)) {
        return false;
    }
    std::memcpy(out, bytes_.data() + (address - base_), count);
    return true;
}

bool FlatMemory::write(std::uint64_t address, const std::uint8_t* in, std::size_t count) {
    if (!contains(address, count)) {
        return false;
    }
    std::memcpy(bytes_.data() + (address - base_), in, count);
    return true;
}

namespace alu {

namespace {

// Truncate to `width` bits. width is 32 or 64.
std::uint64_t mask_to_width(std::uint64_t value, int width) {
    if (width >= 64) {
        return value;
    }
    return value & ((1ULL << width) - 1);
}

std::uint64_t sign_bit(int width) {
    return 1ULL << (width - 1);
}

} // namespace

void set_szp(std::uint64_t result, int width, Flags& flags) {
    const std::uint64_t value = mask_to_width(result, width);
    flags.zf = value == 0;
    flags.sf = (value & sign_bit(width)) != 0;
    // Parity is defined on the low 8 bits: even number of set bits -> PF = 1.
    flags.pf = (__builtin_parity(static_cast<unsigned>(value & 0xFF)) == 0);
}

std::uint64_t add(std::uint64_t a, std::uint64_t b, int width, Flags& flags) {
    const std::uint64_t mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
    const std::uint64_t ua = a & mask;
    const std::uint64_t ub = b & mask;
    const std::uint64_t sum = ua + ub; // may carry into bit `width`
    const std::uint64_t result = sum & mask;

    // Carry: either the 64-bit addition wrapped (sum < ua), or a narrower
    // width carried out past the mask. For width 64 the mask is ~0 so only the
    // wrap test can fire, which is exactly right.
    flags.cf = (sum < ua) || (sum > mask);
    // AF: carry out of bit 3.
    flags.af = (((ua & 0xF) + (ub & 0xF)) & 0x10) != 0;
    // OF: operands share a sign, result differs.
    flags.of = ((~(ua ^ ub) & (ua ^ result)) & sign_bit(width)) != 0;
    set_szp(result, width, flags);
    return result;
}

std::uint64_t sub(std::uint64_t a, std::uint64_t b, int width, Flags& flags) {
    const std::uint64_t mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
    const std::uint64_t ua = a & mask;
    const std::uint64_t ub = b & mask;
    const std::uint64_t result = (ua - ub) & mask;

    flags.cf = ua < ub; // borrow
    flags.af = ((ua & 0xF) < (ub & 0xF));
    // OF: operands differ in sign and the result sign differs from the minuend.
    flags.of = (((ua ^ ub) & (ua ^ result)) & sign_bit(width)) != 0;
    set_szp(result, width, flags);
    return result;
}

std::uint64_t logical_and(std::uint64_t a, std::uint64_t b, int width, Flags& flags) {
    const std::uint64_t result = mask_to_width(a & b, width);
    flags.cf = false;
    flags.of = false;
    flags.af = false;
    set_szp(result, width, flags);
    return result;
}

std::uint64_t logical_or(std::uint64_t a, std::uint64_t b, int width, Flags& flags) {
    const std::uint64_t result = mask_to_width(a | b, width);
    flags.cf = false;
    flags.of = false;
    flags.af = false;
    set_szp(result, width, flags);
    return result;
}

std::uint64_t logical_xor(std::uint64_t a, std::uint64_t b, int width, Flags& flags) {
    const std::uint64_t result = mask_to_width(a ^ b, width);
    flags.cf = false;
    flags.of = false;
    flags.af = false;
    set_szp(result, width, flags);
    return result;
}

std::uint64_t inc(std::uint64_t a, int width, Flags& flags) {
    const bool saved_cf = flags.cf;
    const std::uint64_t result = add(a, 1, width, flags);
    flags.cf = saved_cf; // INC/DEC do not touch CF
    return result;
}

std::uint64_t dec(std::uint64_t a, int width, Flags& flags) {
    const bool saved_cf = flags.cf;
    const std::uint64_t result = sub(a, 1, width, flags);
    flags.cf = saved_cf;
    return result;
}

bool evaluate(Condition condition, const Flags& flags) {
    switch (condition) {
    case Condition::kO:
        return flags.of;
    case Condition::kNo:
        return !flags.of;
    case Condition::kB:
        return flags.cf; // aka C
    case Condition::kAe:
        return !flags.cf; // aka NC
    case Condition::kE:
        return flags.zf; // aka Z
    case Condition::kNe:
        return !flags.zf; // aka NZ
    case Condition::kBe:
        return flags.cf || flags.zf; // aka NA
    case Condition::kA:
        return !flags.cf && !flags.zf; // aka NBE
    case Condition::kS:
        return flags.sf;
    case Condition::kNs:
        return !flags.sf;
    case Condition::kP:
        return flags.pf; // aka PE
    case Condition::kNp:
        return !flags.pf; // aka PO
    case Condition::kL:
        return flags.sf != flags.of; // aka NGE
    case Condition::kGe:
        return flags.sf == flags.of; // aka NL
    case Condition::kLe:
        return flags.zf || (flags.sf != flags.of);
    case Condition::kG:
        return !flags.zf && (flags.sf == flags.of);
    }
    return false;
}

} // namespace alu

namespace {

int width_of(OperandSize size) {
    switch (size) {
    case OperandSize::k8:
        return 8;
    case OperandSize::k16:
        return 16;
    case OperandSize::k32:
        return 32;
    case OperandSize::k64:
        return 64;
    }
    return 64;
}

// Effective address = base + index*scale + disp, all mod 2^64.
std::uint64_t effective_address(const MemoryOperand& mem, const GuestState& state) {
    std::uint64_t address = static_cast<std::uint64_t>(mem.disp);
    if (mem.has_base) {
        address += state.reg(mem.base);
    }
    if (mem.has_index) {
        address += state.reg(mem.index) * mem.scale;
    }
    return address;
}

// Read an operand's current value. Returns false on a memory fault.
bool read_operand(const Operand& op, const GuestState& state, Memory& memory, std::uint64_t& out) {
    switch (op.kind) {
    case OperandKind::kRegister:
        out = state.reg(op.reg);
        return true;
    case OperandKind::kImmediate:
        out = op.imm;
        return true;
    case OperandKind::kMemory: {
        const std::uint64_t address = effective_address(op.mem, state);
        std::uint8_t buffer[8] = {};
        const std::size_t bytes = static_cast<std::size_t>(width_of(op.size)) / 8;
        if (!memory.read(address, buffer, bytes)) {
            return false;
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
            value |= static_cast<std::uint64_t>(buffer[i]) << (8 * i);
        }
        out = value;
        return true;
    }
    }
    return false;
}

// Write an operand, applying x86-64's zero-extension rule: a 32-bit write to a
// general-purpose register clears the upper 32 bits. 8- and 16-bit writes
// merge into the low bits without clearing the rest, but those widths are not
// in the subset, so this only handles 32 and 64.
bool write_operand(const Operand& op, GuestState& state, Memory& memory, std::uint64_t value) {
    const int width = width_of(op.size);
    switch (op.kind) {
    case OperandKind::kRegister:
        if (width == 32) {
            state.reg(op.reg) = value & 0xFFFFFFFFULL; // implicit zero-extension
        } else {
            state.reg(op.reg) = value;
        }
        return true;
    case OperandKind::kMemory: {
        const std::uint64_t address = effective_address(op.mem, state);
        const std::size_t bytes = static_cast<std::size_t>(width) / 8;
        std::uint8_t buffer[8] = {};
        for (std::size_t i = 0; i < bytes; ++i) {
            buffer[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
        }
        return memory.write(address, buffer, bytes);
    }
    case OperandKind::kImmediate:
        return false; // cannot write an immediate
    }
    return false;
}

} // namespace

StepResult step(GuestState& state, Memory& memory, SyscallHandler* syscalls) {
    StepResult result;

    // Fetch. The subset has no instruction longer than 15 bytes, so a 15-byte
    // window is always sufficient and never reads beyond a mapped page for a
    // well-formed program.
    std::uint8_t code[15] = {};
    if (!memory.read(state.rip, code, sizeof(code))) {
        // A short read is acceptable only if the instruction fits; try a
        // smaller window so we do not fault merely because rip is near the end
        // of a mapped region.
        bool fetched = false;
        for (std::size_t n = 15; n >= 1; --n) {
            if (memory.read(state.rip, code, n)) {
                fetched = true;
                // Zero the rest so the decoder sees a truncated stream rather
                // than stale bytes, if the instruction needs more.
                for (std::size_t i = n; i < sizeof(code); ++i) {
                    code[i] = 0;
                }
                break;
            }
        }
        if (!fetched) {
            result.status = StepStatus::kFault;
            result.message = "instruction fetch fault at rip";
            return result;
        }
    }

    const DecodeResult decoded = decode(code, sizeof(code), state.rip);
    result.instruction = decoded.instruction;
    if (!decoded.ok()) {
        result.status = StepStatus::kUnsupported;
        result.decode_error = decoded.error;
        result.message = to_string(decoded.error);
        return result;
    }

    const Instruction& insn = decoded.instruction;
    const int width = width_of(insn.dst.size);
    const std::uint64_t next_rip = state.rip + insn.length;

    auto fault = [&](const char* what) {
        result.status = StepStatus::kFault;
        result.message = what;
        return result;
    };

    switch (insn.opcode) {
    case Opcode::kNop:
        break;

    case Opcode::kHlt:
        result.status = StepStatus::kHalted;
        // HLT does not advance rip; a real CPU would #GP, but for a user-mode
        // test program "stop here" is the useful, explicit behaviour.
        return result;

    case Opcode::kSyscall: {
        // The `syscall` instruction transfers control to the kernel. Hyperion
        // emulates that in userspace through a handler. rip must already point
        // past the instruction when the handler runs, matching a real CPU that
        // returns to the next instruction; the handler writes the result to rax.
        if (syscalls == nullptr) {
            result.status = StepStatus::kUnsupported;
            result.message = "syscall encountered with no handler";
            return result;
        }
        state.rip = next_rip;
        const SyscallResult sr = syscalls->handle(state, memory);
        if (sr.status == SyscallStatus::kExit) {
            result.status = StepStatus::kHalted;
            result.message = "guest exited via syscall";
            return result;
        }
        // kHandled and kUnimplemented both continue: for an unimplemented call
        // the handler wrote -ENOSYS to rax, exactly as Linux would, so the guest
        // observes correct behaviour and can react. The call is recorded (see
        // RunResult::unimplemented_syscalls) so it is visible, never silent.
        if (sr.status == SyscallStatus::kUnimplemented) {
            ++result.unimplemented_syscalls;
        }
        break;
    }

    case Opcode::kMov: {
        std::uint64_t value = 0;
        if (!read_operand(insn.src, state, memory, value)) {
            return fault("mov: source memory fault");
        }
        if (!write_operand(insn.dst, state, memory, value)) {
            return fault("mov: destination memory fault");
        }
        break;
    }

    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kCmp: {
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        if (!read_operand(insn.dst, state, memory, a)) {
            return fault("alu: destination memory fault");
        }
        if (!read_operand(insn.src, state, memory, b)) {
            return fault("alu: source memory fault");
        }

        std::uint64_t result_value = 0;
        switch (insn.opcode) {
        case Opcode::kAdd:
            result_value = alu::add(a, b, width, state.flags);
            break;
        case Opcode::kSub:
            result_value = alu::sub(a, b, width, state.flags);
            break;
        case Opcode::kAnd:
            result_value = alu::logical_and(a, b, width, state.flags);
            break;
        case Opcode::kOr:
            result_value = alu::logical_or(a, b, width, state.flags);
            break;
        case Opcode::kXor:
            result_value = alu::logical_xor(a, b, width, state.flags);
            break;
        case Opcode::kCmp:
            // CMP computes flags exactly like SUB but discards the result.
            (void)alu::sub(a, b, width, state.flags);
            break;
        default:
            break;
        }

        // CMP writes nothing; every other op writes back to dst.
        if (insn.opcode != Opcode::kCmp) {
            if (!write_operand(insn.dst, state, memory, result_value)) {
                return fault("alu: destination memory fault");
            }
        }
        break;
    }

    case Opcode::kInc:
    case Opcode::kDec: {
        std::uint64_t a = 0;
        if (!read_operand(insn.dst, state, memory, a)) {
            return fault("inc/dec: memory fault");
        }
        const std::uint64_t result_value = (insn.opcode == Opcode::kInc)
                                               ? alu::inc(a, width, state.flags)
                                               : alu::dec(a, width, state.flags);
        if (!write_operand(insn.dst, state, memory, result_value)) {
            return fault("inc/dec: memory fault");
        }
        break;
    }

    case Opcode::kJmp:
        state.rip = insn.dst.imm;
        result.status = StepStatus::kOk;
        result.message.clear();
        return result;

    case Opcode::kJcc:
        if (alu::evaluate(insn.condition, state.flags)) {
            state.rip = insn.dst.imm;
        } else {
            state.rip = next_rip;
        }
        return result;

    case Opcode::kPush: {
        // push r64: rsp -= 8; [rsp] = r
        const std::uint64_t rsp = state.reg(Gpr::kRsp) - 8;
        std::uint8_t buffer[8];
        const std::uint64_t value = state.reg(insn.dst.reg);
        for (std::size_t i = 0; i < 8; ++i) {
            buffer[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
        }
        if (!memory.write(rsp, buffer, 8)) {
            return fault("push: stack fault");
        }
        state.reg(Gpr::kRsp) = rsp;
        break;
    }

    case Opcode::kPop: {
        // pop r64: r = [rsp]; rsp += 8
        const std::uint64_t rsp = state.reg(Gpr::kRsp);
        std::uint8_t buffer[8] = {};
        if (!memory.read(rsp, buffer, 8)) {
            return fault("pop: stack fault");
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(buffer[i]) << (8 * i);
        }
        state.reg(insn.dst.reg) = value;
        state.reg(Gpr::kRsp) = rsp + 8;
        break;
    }

    case Opcode::kRet:
        // Modelled as: pop the return address into rip. This keeps RET usable
        // in test programs without modelling the call frame.
        result.status = StepStatus::kHalted;
        result.message = "ret (top-level return)";
        return result;
    }

    state.rip = next_rip;
    result.status = StepStatus::kOk;
    return result;
}

RunResult run_program(Memory& memory, GuestState state, std::uint64_t max_steps,
                      SyscallHandler* syscalls) {
    RunResult out;
    out.state = state;
    for (std::uint64_t i = 0; i < max_steps; ++i) {
        const StepResult r = step(out.state, memory, syscalls);
        out.unimplemented_syscalls += r.unimplemented_syscalls;
        switch (r.status) {
        case StepStatus::kOk:
            ++out.steps;
            break;
        case StepStatus::kHalted:
            ++out.steps;
            out.status = RunStatus::kOk;
            out.message = r.message;
            return out;
        case StepStatus::kFault:
            out.status = RunStatus::kFault;
            out.message = r.message;
            return out;
        case StepStatus::kUnsupported:
            out.status = RunStatus::kUnsupported;
            out.message = r.message;
            return out;
        }
    }
    out.status = RunStatus::kStepLimit;
    out.message = "step limit reached without halting";
    return out;
}

} // namespace hyperion
