// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/interpreter.hpp"

#include "hyperion/syscall.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace hyperion {

namespace {

__extension__ using int128_t = __int128;
__extension__ using uint128_t = unsigned __int128;

} // namespace

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
    ++epoch_;
    return true;
}

PageAccess FlatMemory::translate_page(std::uint64_t page_addr) noexcept {
    if ((base_ & 0xFFFULL) != 0 || (page_addr & 0xFFFULL) != 0) {
        return {};
    }
    if (!contains(page_addr, 4096)) {
        return {};
    }
    PageAccess out;
    out.host_page = bytes_.data() + static_cast<std::size_t>(page_addr - base_);
    out.can_read = true;
    out.can_write_fast = false; // preserve ++epoch_ on FlatMemory self-modifying writes
    return out;
}

namespace alu {

namespace {

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
    flags.pf = (__builtin_parity(static_cast<unsigned>(value & 0xFF)) == 0);
}

std::uint64_t adc(std::uint64_t a, std::uint64_t b, bool cf_in, int width, Flags& flags) {
    const std::uint64_t mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
    const std::uint64_t ua = a & mask;
    const std::uint64_t ub = b & mask;
    const uint128_t wide =
        static_cast<uint128_t>(ua) + static_cast<uint128_t>(ub) + (cf_in ? 1u : 0u);
    const std::uint64_t result = static_cast<std::uint64_t>(wide) & mask;

    flags.cf = wide > mask;
    flags.af = (((ua & 0xFu) + (ub & 0xFu) + (cf_in ? 1u : 0u)) & 0x10u) != 0;
    flags.of = ((~(ua ^ ub) & (ua ^ result)) & sign_bit(width)) != 0;
    set_szp(result, width, flags);
    return result;
}

std::uint64_t add(std::uint64_t a, std::uint64_t b, int width, Flags& flags) {
    return adc(a, b, false, width, flags);
}

std::uint64_t sbb(std::uint64_t a, std::uint64_t b, bool cf_in, int width, Flags& flags) {
    const std::uint64_t mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1);
    const std::uint64_t ua = a & mask;
    const std::uint64_t ub = b & mask;
    const uint128_t subtrahend = static_cast<uint128_t>(ub) + (cf_in ? 1u : 0u);
    const std::uint64_t result =
        static_cast<std::uint64_t>(static_cast<uint128_t>(ua) - subtrahend) & mask;

    flags.cf = static_cast<uint128_t>(ua) < subtrahend;
    flags.af = (ua & 0xFu) < ((ub & 0xFu) + (cf_in ? 1u : 0u));
    flags.of = (((ua ^ ub) & (ua ^ result)) & sign_bit(width)) != 0;
    set_szp(result, width, flags);
    return result;
}

std::uint64_t sub(std::uint64_t a, std::uint64_t b, int width, Flags& flags) {
    return sbb(a, b, false, width, flags);
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
    flags.cf = saved_cf;
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
        return flags.cf;
    case Condition::kAe:
        return !flags.cf;
    case Condition::kE:
        return flags.zf;
    case Condition::kNe:
        return !flags.zf;
    case Condition::kBe:
        return flags.cf || flags.zf;
    case Condition::kA:
        return !flags.cf && !flags.zf;
    case Condition::kS:
        return flags.sf;
    case Condition::kNs:
        return !flags.sf;
    case Condition::kP:
        return flags.pf;
    case Condition::kNp:
        return !flags.pf;
    case Condition::kL:
        return flags.sf != flags.of;
    case Condition::kGe:
        return flags.sf == flags.of;
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
    case OperandSize::k128:
        return 64;
    }
    return 64;
}

std::uint64_t width_mask(int width) {
    return (width >= 64) ? ~0ULL : ((1ULL << width) - 1ULL);
}

std::int64_t sign_extend(std::uint64_t value, int width) {
    if (width >= 64) {
        return static_cast<std::int64_t>(value);
    }
    const int shift = 64 - width;
    return static_cast<std::int64_t>(value << shift) >> shift;
}

std::uint64_t effective_address(const MemoryOperand& mem, const GuestState& state,
                                std::uint64_t next_rip) {
    std::uint64_t address = static_cast<std::uint64_t>(mem.disp);
    if (mem.rip_relative) {
        address += next_rip;
    } else if (mem.has_base) {
        address += state.reg(mem.base);
    }
    if (mem.has_index) {
        address += state.reg(mem.index) * mem.scale;
    }
    if (mem.address_size == OperandSize::k32) {
        address &= 0xFFFFFFFFULL;
    }
    if (mem.fs_relative) {
        address += state.fs_base;
    }
    return address;
}

bool fast_mem_read(Memory& memory, SoftTlb* tlb, std::uint64_t addr, void* out, std::size_t bytes) {
    if (tlb != nullptr) {
        const std::size_t slot = static_cast<std::size_t>((addr >> 12) & kSoftTlbMask);
        SoftTlbEntry& e = tlb->read_entries[slot];
        if ((addr - e.page_tag) <= (4096u - bytes)) {
            const auto* host = std::bit_cast<const std::uint8_t*>(
                static_cast<std::uintptr_t>(addr + e.host_addend));
            std::memcpy(out, host, bytes);
            return true;
        }
        const std::uint64_t page = addr & ~0xFFFULL;
        if ((addr - page) <= (4096u - bytes)) {
            const PageAccess pa = memory.translate_page(page);
            if (pa.host_page != nullptr && pa.can_read) {
                const auto host_addr =
                    static_cast<std::uint64_t>(std::bit_cast<std::uintptr_t>(pa.host_page));
                e.page_tag = page;
                e.host_addend = host_addr - page;
                std::memcpy(out, pa.host_page + static_cast<std::size_t>(addr - page), bytes);
                return true;
            }
        }
    }
    return memory.read(addr, static_cast<std::uint8_t*>(out), bytes);
}

bool fast_mem_write(Memory& memory, SoftTlb* tlb, std::uint64_t addr, const void* in,
                    std::size_t bytes) {
    if (tlb != nullptr) {
        const std::size_t slot = static_cast<std::size_t>((addr >> 12) & kSoftTlbMask);
        SoftTlbEntry& e = tlb->write_entries[slot];
        if ((addr - e.page_tag) <= (4096u - bytes)) {
            auto* host =
                std::bit_cast<std::uint8_t*>(static_cast<std::uintptr_t>(addr + e.host_addend));
            std::memcpy(host, in, bytes);
            return true;
        }
        const std::uint64_t page = addr & ~0xFFFULL;
        if ((addr - page) <= (4096u - bytes)) {
            const PageAccess pa = memory.translate_page(page);
            if (pa.host_page != nullptr && pa.can_write_fast) {
                const auto host_addr =
                    static_cast<std::uint64_t>(std::bit_cast<std::uintptr_t>(pa.host_page));
                e.page_tag = page;
                e.host_addend = host_addr - page;
                std::memcpy(pa.host_page + static_cast<std::size_t>(addr - page), in, bytes);
                return true;
            }
        }
    }
    return memory.write(addr, static_cast<const std::uint8_t*>(in), bytes);
}

std::uint64_t read_gpr_sized(const GuestState& state, Gpr reg, OperandSize size, bool high_byte) {
    const std::uint64_t v = state.reg(reg);
    switch (size) {
    case OperandSize::k8:
        return high_byte ? ((v >> 8) & 0xFFULL) : (v & 0xFFULL);
    case OperandSize::k16:
        return v & 0xFFFFULL;
    case OperandSize::k32:
        return v & 0xFFFFFFFFULL;
    case OperandSize::k64:
    case OperandSize::k128:
        return v;
    }
    return v;
}

void write_gpr_sized(GuestState& state, Gpr reg, OperandSize size, bool high_byte,
                     std::uint64_t value) {
    std::uint64_t& dst = state.reg(reg);
    switch (size) {
    case OperandSize::k8:
        if (high_byte) {
            dst = (dst & ~0xFF00ULL) | ((value & 0xFFULL) << 8);
        } else {
            dst = (dst & ~0xFFULL) | (value & 0xFFULL);
        }
        return;
    case OperandSize::k16:
        dst = (dst & ~0xFFFFULL) | (value & 0xFFFFULL);
        return;
    case OperandSize::k32:
        dst = value & 0xFFFFFFFFULL;
        return;
    case OperandSize::k64:
    case OperandSize::k128:
        dst = value;
        return;
    }
}

bool read_operand(const Operand& op, const GuestState& state, Memory& memory, SoftTlb* tlb,
                  std::uint64_t next_rip, std::uint64_t& out) {
    switch (op.kind) {
    case OperandKind::kRegister:
        out = read_gpr_sized(state, op.reg, op.size, op.high_byte);
        return true;
    case OperandKind::kImmediate:
        out = op.imm;
        return true;
    case OperandKind::kMemory: {
        const std::uint64_t address = effective_address(op.mem, state, next_rip);
        std::uint64_t value = 0;
        const auto bytes = static_cast<std::size_t>(width_of(op.size)) / 8;
        if (!fast_mem_read(memory, tlb, address, &value, bytes)) {
            return false;
        }
        out = value;
        return true;
    }
    case OperandKind::kXmm:
        out = state.xmm[op.xmm].lo;
        return true;
    }
    return false;
}

bool write_operand(const Operand& op, GuestState& state, Memory& memory, SoftTlb* tlb,
                   std::uint64_t next_rip, std::uint64_t value) {
    switch (op.kind) {
    case OperandKind::kRegister:
        write_gpr_sized(state, op.reg, op.size, op.high_byte, value);
        return true;
    case OperandKind::kMemory: {
        const std::uint64_t address = effective_address(op.mem, state, next_rip);
        const auto bytes = static_cast<std::size_t>(width_of(op.size)) / 8;
        return fast_mem_write(memory, tlb, address, &value, bytes);
    }
    case OperandKind::kImmediate:
    case OperandKind::kXmm:
        return false;
    }
    return false;
}

} // namespace

void execute_cpuid(GuestState& state) noexcept {
    const auto leaf = static_cast<std::uint32_t>(state.reg(Gpr::kRax));
    const auto sub = static_cast<std::uint32_t>(state.reg(Gpr::kRcx));
    std::uint32_t eax = 0;
    std::uint32_t ebx = 0;
    std::uint32_t ecx = 0;
    std::uint32_t edx = 0;

    if (leaf == 0) {
        eax = 7u;
        ebx = 0x756E6547u; // "Genu"
        edx = 0x49656E69u; // "ineI"
        ecx = 0x6C65746Eu; // "ntel"
    } else if (leaf == 1) {
        eax = 0x000006F1u;
        ebx = 0x00010800u; // CLFLUSH 64B line
        ecx = 0u;          // baseline x86-64: no SSE3/SSSE3/SSE4/AVX
        edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 24) |
              (1u << 25) | (1u << 26); // FPU, TSC, CX8, CMOV, MMX, FXSR, SSE, SSE2
    } else if (leaf == 7 && sub == 0) {
        eax = 0u;
        ebx = 0u;
        ecx = 0u;
        edx = 0u;
    } else if (leaf == 0x80000000u) {
        eax = 0x80000008u;
    } else if (leaf == 0x80000001u) {
        edx = (1u << 20) | (1u << 29); // NX, LM
    } else if (leaf == 0x80000008u) {
        eax = 0x00003028u;
    }

    state.reg(Gpr::kRax) = eax;
    state.reg(Gpr::kRbx) = ebx;
    state.reg(Gpr::kRcx) = ecx;
    state.reg(Gpr::kRdx) = edx;
}

bool execute_string_instruction(const Instruction& insn, GuestState& state, Memory& memory,
                                SoftTlb* tlb) {
    const int width = width_of(insn.dst.size);
    const auto elem_bytes = static_cast<std::size_t>(width / 8);
    const std::int64_t step_delta =
        state.df ? -static_cast<std::int64_t>(elem_bytes) : static_cast<std::int64_t>(elem_bytes);
    const bool has_rep = (insn.rep == 0xF2 || insn.rep == 0xF3);

    auto run_once = [&]() -> bool {
        switch (insn.subop) {
        case 0xA4:
        case 0xA5: { // MOVS [rdi], [rsi]
            std::uint64_t v = 0;
            if (!fast_mem_read(memory, tlb, state.reg(Gpr::kRsi), &v, elem_bytes)) {
                return false;
            }
            if (!fast_mem_write(memory, tlb, state.reg(Gpr::kRdi), &v, elem_bytes)) {
                return false;
            }
            state.reg(Gpr::kRsi) += static_cast<std::uint64_t>(step_delta);
            state.reg(Gpr::kRdi) += static_cast<std::uint64_t>(step_delta);
            return true;
        }
        case 0xAA:
        case 0xAB: { // STOS [rdi], rax
            const std::uint64_t v = state.reg(Gpr::kRax);
            if (!fast_mem_write(memory, tlb, state.reg(Gpr::kRdi), &v, elem_bytes)) {
                return false;
            }
            state.reg(Gpr::kRdi) += static_cast<std::uint64_t>(step_delta);
            return true;
        }
        case 0xA6:
        case 0xA7: { // CMPS [rsi], [rdi]
            std::uint64_t a = 0;
            std::uint64_t b = 0;
            if (!fast_mem_read(memory, tlb, state.reg(Gpr::kRsi), &a, elem_bytes) ||
                !fast_mem_read(memory, tlb, state.reg(Gpr::kRdi), &b, elem_bytes)) {
                return false;
            }
            (void)alu::sub(a, b, width, state.flags);
            state.reg(Gpr::kRsi) += static_cast<std::uint64_t>(step_delta);
            state.reg(Gpr::kRdi) += static_cast<std::uint64_t>(step_delta);
            return true;
        }
        case 0xAE:
        case 0xAF: { // SCAS rax, [rdi]
            std::uint64_t b = 0;
            if (!fast_mem_read(memory, tlb, state.reg(Gpr::kRdi), &b, elem_bytes)) {
                return false;
            }
            (void)alu::sub(state.reg(Gpr::kRax), b, width, state.flags);
            state.reg(Gpr::kRdi) += static_cast<std::uint64_t>(step_delta);
            return true;
        }
        default:
            return false;
        }
    };

    if (!has_rep) {
        return run_once();
    }

    const bool check_zf =
        (insn.subop == 0xA6 || insn.subop == 0xA7 || insn.subop == 0xAE || insn.subop == 0xAF);
    while (state.reg(Gpr::kRcx) != 0) {
        if (!run_once()) {
            return false;
        }
        state.reg(Gpr::kRcx) -= 1;
        if (check_zf) {
            if (insn.rep == 0xF3 && !state.flags.zf) {
                break;
            }
            if (insn.rep == 0xF2 && state.flags.zf) {
                break;
            }
        }
    }
    return true;
}

bool execute_sse_instruction(const Instruction& insn, GuestState& state, Memory& memory,
                             SoftTlb* tlb) {
    const std::uint64_t next_rip = state.rip + insn.length;

    auto load_xmm_src = [&](XmmReg& out) -> bool {
        if (insn.src.kind == OperandKind::kXmm) {
            out = state.xmm[insn.src.xmm];
            return true;
        }
        const std::uint64_t addr = effective_address(insn.src.mem, state, next_rip);
        std::uint8_t raw[16];
        if (!fast_mem_read(memory, tlb, addr, raw, 16)) {
            return false;
        }
        std::memcpy(&out.lo, raw, 8);
        std::memcpy(&out.hi, raw + 8, 8);
        return true;
    };

    auto to_bytes = [](const XmmReg& r, void* out) {
        auto* p = static_cast<std::uint8_t*>(out);
        std::memcpy(p, &r.lo, 8);
        std::memcpy(p + 8, &r.hi, 8);
    };
    auto from_bytes = [](const void* in) {
        const auto* p = static_cast<const std::uint8_t*>(in);
        XmmReg r{};
        std::memcpy(&r.lo, p, 8);
        std::memcpy(&r.hi, p + 8, 8);
        return r;
    };

    switch (insn.sse_op) {
    case SseOp::kMovUps: {
        if (insn.dst.kind == OperandKind::kXmm) {
            return load_xmm_src(state.xmm[insn.dst.xmm]);
        }
        const XmmReg src = state.xmm[insn.src.xmm];
        std::uint8_t raw[16];
        std::memcpy(raw, &src.lo, 8);
        std::memcpy(raw + 8, &src.hi, 8);
        const std::uint64_t addr = effective_address(insn.dst.mem, state, next_rip);
        return fast_mem_write(memory, tlb, addr, raw, 16);
    }
    case SseOp::kMovQ: {
        if (insn.dst.kind == OperandKind::kXmm) {
            std::uint64_t v = 0;
            if (insn.src.kind == OperandKind::kXmm) {
                v = state.xmm[insn.src.xmm].lo;
            } else if (!read_operand(insn.src, state, memory, tlb, next_rip, v)) {
                return false;
            }
            state.xmm[insn.dst.xmm] = XmmReg{v, 0};
            return true;
        }
        const std::uint64_t v = state.xmm[insn.src.xmm].lo;
        if (insn.dst.kind == OperandKind::kXmm) {
            state.xmm[insn.dst.xmm] = XmmReg{v, 0};
            return true;
        }
        return write_operand(insn.dst, state, memory, tlb, next_rip, v);
    }
    case SseOp::kMovD: {
        if (insn.dst.kind == OperandKind::kXmm) {
            std::uint64_t v = 0;
            if (insn.src.kind == OperandKind::kXmm) {
                v = state.xmm[insn.src.xmm].lo & 0xFFFFFFFFULL;
                state.xmm[insn.dst.xmm].lo =
                    (state.xmm[insn.dst.xmm].lo & ~0xFFFFFFFFULL) | v; // MOVSS reg,reg merges!
                return true;
            }
            if (!read_operand(insn.src, state, memory, tlb, next_rip, v)) {
                return false;
            }
            state.xmm[insn.dst.xmm] = XmmReg{v & 0xFFFFFFFFULL, 0};
            return true;
        }
        const std::uint64_t v = state.xmm[insn.src.xmm].lo & 0xFFFFFFFFULL;
        return write_operand(insn.dst, state, memory, tlb, next_rip, v);
    }
    case SseOp::kMovLpd: {
        if (insn.dst.kind == OperandKind::kXmm) {
            std::uint64_t v = 0;
            if (!read_operand(insn.src, state, memory, tlb, next_rip, v)) {
                return false;
            }
            state.xmm[insn.dst.xmm].lo = v;
            return true;
        }
        return write_operand(insn.dst, state, memory, tlb, next_rip, state.xmm[insn.src.xmm].lo);
    }
    case SseOp::kMovHpd: {
        if (insn.dst.kind == OperandKind::kXmm) {
            std::uint64_t v = 0;
            if (!read_operand(insn.src, state, memory, tlb, next_rip, v)) {
                return false;
            }
            state.xmm[insn.dst.xmm].hi = v;
            return true;
        }
        return write_operand(insn.dst, state, memory, tlb, next_rip, state.xmm[insn.src.xmm].hi);
    }
    case SseOp::kMovLhps:
        state.xmm[insn.dst.xmm].hi = state.xmm[insn.src.xmm].lo;
        return true;
    case SseOp::kMovHlps:
        state.xmm[insn.dst.xmm].lo = state.xmm[insn.src.xmm].hi;
        return true;
    case SseOp::kPxor: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        state.xmm[insn.dst.xmm].lo ^= s.lo;
        state.xmm[insn.dst.xmm].hi ^= s.hi;
        return true;
    }
    case SseOp::kPor: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        state.xmm[insn.dst.xmm].lo |= s.lo;
        state.xmm[insn.dst.xmm].hi |= s.hi;
        return true;
    }
    case SseOp::kPand: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        state.xmm[insn.dst.xmm].lo &= s.lo;
        state.xmm[insn.dst.xmm].hi &= s.hi;
        return true;
    }
    case SseOp::kPandn: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        XmmReg& d = state.xmm[insn.dst.xmm];
        d.lo = (~d.lo) & s.lo;
        d.hi = (~d.hi) & s.hi;
        return true;
    }
    case SseOp::kPmovmskb: {
        std::array<std::uint8_t, 16> b{};
        to_bytes(state.xmm[insn.src.xmm], b.data());
        std::uint32_t mask = 0;
        for (unsigned i = 0; i < 16; ++i) {
            mask |= static_cast<std::uint32_t>((b[i] >> 7) & 1u) << i;
        }
        state.reg(insn.dst.reg) = mask;
        return true;
    }
    case SseOp::kPcmpeqb:
    case SseOp::kPcmpgtb:
    case SseOp::kPaddb:
    case SseOp::kPsubb:
    case SseOp::kPminub:
    case SseOp::kPmaxub: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        std::array<std::uint8_t, 16> a{}, b{}, o{};
        to_bytes(state.xmm[insn.dst.xmm], a.data());
        to_bytes(s, b.data());
        for (std::size_t i = 0; i < 16; ++i) {
            switch (insn.sse_op) {
            case SseOp::kPcmpeqb:
                o[i] = (a[i] == b[i]) ? 0xFFu : 0x00u;
                break;
            case SseOp::kPcmpgtb:
                o[i] = (static_cast<std::int8_t>(a[i]) > static_cast<std::int8_t>(b[i])) ? 0xFFu
                                                                                         : 0x00u;
                break;
            case SseOp::kPaddb:
                o[i] = static_cast<std::uint8_t>(a[i] + b[i]);
                break;
            case SseOp::kPsubb:
                o[i] = static_cast<std::uint8_t>(a[i] - b[i]);
                break;
            case SseOp::kPminub:
                o[i] = std::min(a[i], b[i]);
                break;
            default:
                o[i] = std::max(a[i], b[i]);
                break;
            }
        }
        state.xmm[insn.dst.xmm] = from_bytes(o.data());
        return true;
    }
    case SseOp::kPcmpeqw:
    case SseOp::kPcmpgtw:
    case SseOp::kPaddw:
    case SseOp::kPsubw: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        std::array<std::uint16_t, 8> a{}, b{}, o{};
        to_bytes(state.xmm[insn.dst.xmm], a.data());
        to_bytes(s, b.data());
        for (std::size_t i = 0; i < 8; ++i) {
            switch (insn.sse_op) {
            case SseOp::kPcmpeqw:
                o[i] = (a[i] == b[i]) ? 0xFFFFu : 0x0000u;
                break;
            case SseOp::kPcmpgtw:
                o[i] = (static_cast<std::int16_t>(a[i]) > static_cast<std::int16_t>(b[i]))
                           ? 0xFFFFu
                           : 0x0000u;
                break;
            case SseOp::kPaddw:
                o[i] = static_cast<std::uint16_t>(a[i] + b[i]);
                break;
            default:
                o[i] = static_cast<std::uint16_t>(a[i] - b[i]);
                break;
            }
        }
        state.xmm[insn.dst.xmm] = from_bytes(o.data());
        return true;
    }
    case SseOp::kPcmpeqd:
    case SseOp::kPcmpgtd:
    case SseOp::kPaddd:
    case SseOp::kPsubd: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        std::array<std::uint32_t, 4> a{}, b{}, o{};
        to_bytes(state.xmm[insn.dst.xmm], a.data());
        to_bytes(s, b.data());
        for (std::size_t i = 0; i < 4; ++i) {
            switch (insn.sse_op) {
            case SseOp::kPcmpeqd:
                o[i] = (a[i] == b[i]) ? 0xFFFFFFFFu : 0u;
                break;
            case SseOp::kPcmpgtd:
                o[i] = (static_cast<std::int32_t>(a[i]) > static_cast<std::int32_t>(b[i]))
                           ? 0xFFFFFFFFu
                           : 0u;
                break;
            case SseOp::kPaddd:
                o[i] = a[i] + b[i];
                break;
            default:
                o[i] = a[i] - b[i];
                break;
            }
        }
        state.xmm[insn.dst.xmm] = from_bytes(o.data());
        return true;
    }
    case SseOp::kPaddq: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        state.xmm[insn.dst.xmm].lo += s.lo;
        state.xmm[insn.dst.xmm].hi += s.hi;
        return true;
    }
    case SseOp::kPsubq: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        state.xmm[insn.dst.xmm].lo -= s.lo;
        state.xmm[insn.dst.xmm].hi -= s.hi;
        return true;
    }
    case SseOp::kPunpcklbw:
    case SseOp::kPunpckhbw: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        std::array<std::uint8_t, 16> a{}, b{}, o{};
        to_bytes(state.xmm[insn.dst.xmm], a.data());
        to_bytes(s, b.data());
        const std::size_t base = (insn.sse_op == SseOp::kPunpckhbw) ? 8 : 0;
        for (std::size_t i = 0; i < 8; ++i) {
            o[2 * i] = a[base + i];
            o[2 * i + 1] = b[base + i];
        }
        state.xmm[insn.dst.xmm] = from_bytes(o.data());
        return true;
    }
    case SseOp::kPunpcklwd:
    case SseOp::kPunpckhwd: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        std::array<std::uint16_t, 8> a{}, b{}, o{};
        to_bytes(state.xmm[insn.dst.xmm], a.data());
        to_bytes(s, b.data());
        const std::size_t base = (insn.sse_op == SseOp::kPunpckhwd) ? 4 : 0;
        for (std::size_t i = 0; i < 4; ++i) {
            o[2 * i] = a[base + i];
            o[2 * i + 1] = b[base + i];
        }
        state.xmm[insn.dst.xmm] = from_bytes(o.data());
        return true;
    }
    case SseOp::kPunpckldq:
    case SseOp::kPunpckhdq: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        std::array<std::uint32_t, 4> a{}, b{}, o{};
        to_bytes(state.xmm[insn.dst.xmm], a.data());
        to_bytes(s, b.data());
        const std::size_t base = (insn.sse_op == SseOp::kPunpckhdq) ? 2 : 0;
        o[0] = a[base];
        o[1] = b[base];
        o[2] = a[base + 1];
        o[3] = b[base + 1];
        state.xmm[insn.dst.xmm] = from_bytes(o.data());
        return true;
    }
    case SseOp::kPunpcklqdq: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        state.xmm[insn.dst.xmm].hi = s.lo;
        return true;
    }
    case SseOp::kPunpckhqdq: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        XmmReg& d = state.xmm[insn.dst.xmm];
        d = XmmReg{d.hi, s.hi};
        return true;
    }
    case SseOp::kPackuswb:
    case SseOp::kPacksswb: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        std::array<std::int16_t, 8> a{}, b{};
        to_bytes(state.xmm[insn.dst.xmm], a.data());
        to_bytes(s, b.data());
        std::array<std::uint8_t, 16> o{};
        auto sat = [&](std::int16_t v) -> std::uint8_t {
            if (insn.sse_op == SseOp::kPackuswb) {
                return v < 0 ? 0u : (v > 255 ? 255u : static_cast<std::uint8_t>(v));
            }
            const auto c = v < -128 ? static_cast<std::int16_t>(-128)
                                    : (v > 127 ? static_cast<std::int16_t>(127) : v);
            return static_cast<std::uint8_t>(c);
        };
        for (std::size_t i = 0; i < 8; ++i) {
            o[i] = sat(a[i]);
            o[8 + i] = sat(b[i]);
        }
        state.xmm[insn.dst.xmm] = from_bytes(o.data());
        return true;
    }
    case SseOp::kPackssdw: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        std::array<std::int32_t, 4> a{}, b{};
        to_bytes(state.xmm[insn.dst.xmm], a.data());
        to_bytes(s, b.data());
        std::array<std::int16_t, 8> o{};
        auto sat = [](std::int32_t v) -> std::int16_t {
            return v < -32768 ? static_cast<std::int16_t>(-32768)
                              : (v > 32767 ? static_cast<std::int16_t>(32767)
                                           : static_cast<std::int16_t>(v));
        };
        for (std::size_t i = 0; i < 4; ++i) {
            o[i] = sat(a[i]);
            o[4 + i] = sat(b[i]);
        }
        state.xmm[insn.dst.xmm] = from_bytes(o.data());
        return true;
    }
    case SseOp::kPshufd: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        std::array<std::uint32_t, 4> in{}, o{};
        to_bytes(s, in.data());
        const auto imm = static_cast<unsigned>(insn.aux.imm);
        for (unsigned i = 0; i < 4; ++i) {
            o[i] = in[static_cast<std::size_t>((imm >> (2 * i)) & 3u)];
        }
        state.xmm[insn.dst.xmm] = from_bytes(o.data());
        return true;
    }
    case SseOp::kPshuflw:
    case SseOp::kPshufhw: {
        XmmReg s;
        if (!load_xmm_src(s)) {
            return false;
        }
        std::array<std::uint16_t, 8> in{}, o{};
        to_bytes(s, in.data());
        o = in;
        const auto imm = static_cast<unsigned>(insn.aux.imm);
        const std::size_t base = (insn.sse_op == SseOp::kPshufhw) ? 4 : 0;
        for (unsigned i = 0; i < 4; ++i) {
            o[base + i] = in[base + static_cast<std::size_t>((imm >> (2 * i)) & 3u)];
        }
        state.xmm[insn.dst.xmm] = from_bytes(o.data());
        return true;
    }
    case SseOp::kPsrlwImm:
    case SseOp::kPsrawImm:
    case SseOp::kPsllwImm: {
        std::array<std::uint16_t, 8> a{};
        to_bytes(state.xmm[insn.dst.xmm], a.data());
        const auto c = static_cast<unsigned>(insn.src.imm);
        for (std::size_t i = 0; i < 8; ++i) {
            if (insn.sse_op == SseOp::kPsrlwImm) {
                a[i] = (c > 15) ? static_cast<std::uint16_t>(0)
                                : static_cast<std::uint16_t>(a[i] >> c);
            } else if (insn.sse_op == SseOp::kPsllwImm) {
                a[i] = (c > 15) ? static_cast<std::uint16_t>(0)
                                : static_cast<std::uint16_t>(a[i] << c);
            } else {
                const auto sv = static_cast<std::int16_t>(a[i]);
                a[i] = static_cast<std::uint16_t>(sv >> (c > 15 ? 15u : c));
            }
        }
        state.xmm[insn.dst.xmm] = from_bytes(a.data());
        return true;
    }
    case SseOp::kPsrldImm:
    case SseOp::kPsradImm:
    case SseOp::kPslldImm: {
        std::array<std::uint32_t, 4> a{};
        to_bytes(state.xmm[insn.dst.xmm], a.data());
        const auto c = static_cast<unsigned>(insn.src.imm);
        for (std::size_t i = 0; i < 4; ++i) {
            if (insn.sse_op == SseOp::kPsrldImm) {
                a[i] = (c > 31) ? 0u : (a[i] >> c);
            } else if (insn.sse_op == SseOp::kPslldImm) {
                a[i] = (c > 31) ? 0u : (a[i] << c);
            } else {
                const auto sv = static_cast<std::int32_t>(a[i]);
                a[i] = static_cast<std::uint32_t>(sv >> (c > 31 ? 31u : c));
            }
        }
        state.xmm[insn.dst.xmm] = from_bytes(a.data());
        return true;
    }
    case SseOp::kPsrlqImm:
    case SseOp::kPsllqImm: {
        XmmReg& d = state.xmm[insn.dst.xmm];
        const auto c = static_cast<unsigned>(insn.src.imm);
        if (c > 63) {
            d = XmmReg{0, 0};
        } else if (insn.sse_op == SseOp::kPsrlqImm) {
            d.lo >>= c;
            d.hi >>= c;
        } else {
            d.lo <<= c;
            d.hi <<= c;
        }
        return true;
    }
    case SseOp::kPsrldqImm:
    case SseOp::kPslldqImm: {
        std::array<std::uint8_t, 16> a{}, o{};
        to_bytes(state.xmm[insn.dst.xmm], a.data());
        const auto c = static_cast<std::size_t>(insn.src.imm);
        if (c < 16) {
            if (insn.sse_op == SseOp::kPsrldqImm) {
                for (std::size_t i = 0; i + c < 16; ++i) {
                    o[i] = a[i + c];
                }
            } else {
                for (std::size_t i = c; i < 16; ++i) {
                    o[i] = a[i - c];
                }
            }
        }
        state.xmm[insn.dst.xmm] = from_bytes(o.data());
        return true;
    }
    case SseOp::kNone:
        return false;
    }
    return false;
}

namespace {

struct FastStepOutcome {
    StepStatus status = StepStatus::kOk;
    std::uint32_t unimplemented_syscalls = 0;
    bool exited_via_syscall = false;
    std::int64_t exit_code = 0;
    const char* message = nullptr;
};

FastStepOutcome execute_decoded(GuestState& state, Memory& memory, SoftTlb* tlb,
                                const Instruction& insn, SyscallHandler* syscalls) {
    FastStepOutcome result;
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
        return result;

    case Opcode::kCpuid:
        execute_cpuid(state);
        break;

    case Opcode::kRdtsc:
        // Deterministic monotonic user-mode timestamp counter (RDX=0, RAX=0).
        state.reg(Gpr::kRax) = 0;
        state.reg(Gpr::kRdx) = 0;
        break;

    case Opcode::kCld:
        state.df = (insn.subop != 0);
        break;

    case Opcode::kStringOp:
        if (!execute_string_instruction(insn, state, memory, tlb)) {
            return fault("string op: memory fault");
        }
        break;

    case Opcode::kSse:
        if (!execute_sse_instruction(insn, state, memory, tlb)) {
            return fault("sse2: memory fault");
        }
        break;

    case Opcode::kSyscall: {
        if (syscalls == nullptr) {
            result.status = StepStatus::kUnsupported;
            result.message = "syscall encountered with no handler";
            return result;
        }
        state.rip = next_rip;
        const SyscallResult sr = syscalls->handle(state, memory);
        if (tlb != nullptr) {
            tlb->flush(memory.code_epoch());
        }
        if (sr.status == SyscallStatus::kExit) {
            result.status = StepStatus::kHalted;
            result.exited_via_syscall = true;
            result.exit_code = sr.exit_code;
            result.message = "guest exited via syscall";
            return result;
        }
        if (sr.status == SyscallStatus::kUnimplemented) {
            ++result.unimplemented_syscalls;
        }
        return result;
    }

    case Opcode::kMov:
    case Opcode::kMovzx: {
        std::uint64_t value = 0;
        if (!read_operand(insn.src, state, memory, tlb, next_rip, value)) {
            return fault("mov: source memory fault");
        }
        if (!write_operand(insn.dst, state, memory, tlb, next_rip, value)) {
            return fault("mov: destination memory fault");
        }
        break;
    }

    case Opcode::kMovsx: {
        std::uint64_t raw = 0;
        if (!read_operand(insn.src, state, memory, tlb, next_rip, raw)) {
            return fault("movsx: source memory fault");
        }
        const auto sext = static_cast<std::uint64_t>(sign_extend(raw, width_of(insn.src.size)));
        if (!write_operand(insn.dst, state, memory, tlb, next_rip, sext)) {
            return fault("movsx: destination fault");
        }
        break;
    }

    case Opcode::kCdqe: {
        const int src_w = width / 2;
        const std::uint64_t sext = static_cast<std::uint64_t>(
            sign_extend(state.reg(Gpr::kRax) & width_mask(src_w), src_w));
        write_gpr_sized(state, Gpr::kRax, insn.dst.size, false, sext);
        break;
    }

    case Opcode::kCqo: {
        const std::int64_t sv = sign_extend(state.reg(Gpr::kRax) & width_mask(width), width);
        const std::uint64_t ext = (sv < 0) ? ~0ULL : 0ULL;
        write_gpr_sized(state, Gpr::kRdx, insn.dst.size, false, ext);
        break;
    }

    case Opcode::kLea: {
        const std::uint64_t addr = effective_address(insn.src.mem, state, next_rip);
        if (!write_operand(insn.dst, state, memory, tlb, next_rip, addr)) {
            return fault("lea: destination fault");
        }
        break;
    }

    case Opcode::kAdd:
    case Opcode::kAdc:
    case Opcode::kSub:
    case Opcode::kSbb:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kXor:
    case Opcode::kCmp:
    case Opcode::kTest: {
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        if (!read_operand(insn.dst, state, memory, tlb, next_rip, a)) {
            return fault("alu: destination memory fault");
        }
        if (!read_operand(insn.src, state, memory, tlb, next_rip, b)) {
            return fault("alu: source memory fault");
        }

        std::uint64_t result_value = 0;
        switch (insn.opcode) {
        case Opcode::kAdd:
            result_value = alu::add(a, b, width, state.flags);
            break;
        case Opcode::kAdc:
            result_value = alu::adc(a, b, state.flags.cf, width, state.flags);
            break;
        case Opcode::kSub:
            result_value = alu::sub(a, b, width, state.flags);
            break;
        case Opcode::kSbb:
            result_value = alu::sbb(a, b, state.flags.cf, width, state.flags);
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
            (void)alu::sub(a, b, width, state.flags);
            break;
        case Opcode::kTest:
            (void)alu::logical_and(a, b, width, state.flags);
            break;
        default:
            break;
        }

        if (insn.opcode != Opcode::kCmp && insn.opcode != Opcode::kTest) {
            if (!write_operand(insn.dst, state, memory, tlb, next_rip, result_value)) {
                return fault("alu: destination memory fault");
            }
        }
        break;
    }

    case Opcode::kInc:
    case Opcode::kDec:
    case Opcode::kNot:
    case Opcode::kNeg: {
        std::uint64_t a = 0;
        if (!read_operand(insn.dst, state, memory, tlb, next_rip, a)) {
            return fault("unary alu: memory fault");
        }
        std::uint64_t result_value = 0;
        switch (insn.opcode) {
        case Opcode::kInc:
            result_value = alu::inc(a, width, state.flags);
            break;
        case Opcode::kDec:
            result_value = alu::dec(a, width, state.flags);
            break;
        case Opcode::kNot:
            result_value = (~a) & width_mask(width);
            break;
        default:
            result_value = alu::sub(0, a, width, state.flags);
            break;
        }
        if (!write_operand(insn.dst, state, memory, tlb, next_rip, result_value)) {
            return fault("unary alu: memory fault");
        }
        break;
    }

    case Opcode::kShl:
    case Opcode::kShr:
    case Opcode::kSar:
    case Opcode::kRol:
    case Opcode::kRor: {
        std::uint64_t a = 0;
        std::uint64_t raw_cnt = 0;
        if (!read_operand(insn.dst, state, memory, tlb, next_rip, a) ||
            !read_operand(insn.src, state, memory, tlb, next_rip, raw_cnt)) {
            return fault("shift: memory fault");
        }
        const auto uwidth = static_cast<unsigned>(width);
        const unsigned mask_bits = (width == 64) ? 63u : 31u;
        const auto cnt = static_cast<unsigned>(raw_cnt & mask_bits);
        const std::uint64_t wmask = width_mask(width);
        const std::uint64_t ua = a & wmask;
        std::uint64_t res = ua;

        if (cnt != 0) {
            switch (insn.opcode) {
            case Opcode::kShl:
                res = (ua << cnt) & wmask;
                state.flags.cf = ((ua >> (uwidth - cnt)) & 1ULL) != 0;
                if (cnt == 1) {
                    state.flags.of = (((res >> (uwidth - 1)) & 1ULL) != 0) != state.flags.cf;
                }
                alu::set_szp(res, width, state.flags);
                break;
            case Opcode::kShr:
                res = ua >> cnt;
                state.flags.cf = ((ua >> (cnt - 1)) & 1ULL) != 0;
                if (cnt == 1) {
                    state.flags.of = ((ua >> (uwidth - 1)) & 1ULL) != 0;
                }
                alu::set_szp(res, width, state.flags);
                break;
            case Opcode::kSar: {
                const std::int64_t sa = sign_extend(ua, width);
                res = static_cast<std::uint64_t>(sa >> cnt) & wmask;
                state.flags.cf = ((ua >> (cnt - 1)) & 1ULL) != 0;
                if (cnt == 1) {
                    state.flags.of = false;
                }
                alu::set_szp(res, width, state.flags);
                break;
            }
            case Opcode::kRol: {
                const unsigned eff = cnt % uwidth;
                res = (eff == 0) ? ua : (((ua << eff) | (ua >> (uwidth - eff))) & wmask);
                state.flags.cf = (res & 1ULL) != 0;
                if (cnt == 1) {
                    state.flags.of = (((res >> (uwidth - 1)) & 1ULL) != 0) != state.flags.cf;
                }
                break;
            }
            case Opcode::kRor: {
                const unsigned eff = cnt % uwidth;
                res = (eff == 0) ? ua : (((ua >> eff) | (ua << (uwidth - eff))) & wmask);
                state.flags.cf = ((res >> (uwidth - 1)) & 1ULL) != 0;
                if (cnt == 1) {
                    state.flags.of =
                        (((res >> (uwidth - 1)) & 1ULL) ^ ((res >> (uwidth - 2)) & 1ULL)) != 0;
                }
                break;
            }
            default:
                break;
            }
        }
        if (!write_operand(insn.dst, state, memory, tlb, next_rip, res)) {
            return fault("shift: write fault");
        }
        break;
    }

    case Opcode::kShld:
    case Opcode::kShrd: {
        std::uint64_t a = 0;
        std::uint64_t fill = 0;
        std::uint64_t raw_cnt = 0;
        if (!read_operand(insn.dst, state, memory, tlb, next_rip, a) ||
            !read_operand(insn.src, state, memory, tlb, next_rip, fill) ||
            !read_operand(insn.aux, state, memory, tlb, next_rip, raw_cnt)) {
            return fault("shld/shrd: memory fault");
        }
        const auto uwidth = static_cast<unsigned>(width);
        const unsigned mask_bits = (width == 64) ? 63u : 31u;
        const auto cnt = static_cast<unsigned>(raw_cnt & mask_bits);
        const std::uint64_t wmask = width_mask(width);
        std::uint64_t res = a & wmask;
        if (cnt != 0 && cnt <= uwidth) {
            if (insn.opcode == Opcode::kShld) {
                res = (((a & wmask) << cnt) | ((fill & wmask) >> (uwidth - cnt))) & wmask;
                state.flags.cf = (((a & wmask) >> (uwidth - cnt)) & 1ULL) != 0;
            } else {
                res = (((a & wmask) >> cnt) | ((fill & wmask) << (uwidth - cnt))) & wmask;
                state.flags.cf = (((a & wmask) >> (cnt - 1)) & 1ULL) != 0;
            }
            alu::set_szp(res, width, state.flags);
        }
        if (!write_operand(insn.dst, state, memory, tlb, next_rip, res)) {
            return fault("shld/shrd: write fault");
        }
        break;
    }

    case Opcode::kMul: {
        std::uint64_t src_val = 0;
        if (!read_operand(insn.dst, state, memory, tlb, next_rip, src_val)) {
            return fault("mul: source fault");
        }
        const std::uint64_t wmask = width_mask(width);
        const uint128_t prod = static_cast<uint128_t>(state.reg(Gpr::kRax) & wmask) *
                               static_cast<uint128_t>(src_val & wmask);
        const std::uint64_t lo = static_cast<std::uint64_t>(prod) & wmask;
        const std::uint64_t hi = static_cast<std::uint64_t>(prod >> width) & wmask;
        if (width == 8) {
            write_gpr_sized(state, Gpr::kRax, OperandSize::k16, false, (hi << 8) | lo);
        } else {
            write_gpr_sized(state, Gpr::kRax, insn.dst.size, false, lo);
            write_gpr_sized(state, Gpr::kRdx, insn.dst.size, false, hi);
        }
        state.flags.cf = (hi != 0);
        state.flags.of = state.flags.cf;
        break;
    }

    case Opcode::kImul: {
        if (!insn.has_src) {
            // 1-operand widening signed multiply: RDX:RAX = sign_ext(RAX) * sign_ext(src)
            std::uint64_t src_val = 0;
            if (!read_operand(insn.dst, state, memory, tlb, next_rip, src_val)) {
                return fault("imul: source fault");
            }
            const int128_t sa = sign_extend(state.reg(Gpr::kRax), width);
            const int128_t sb = sign_extend(src_val, width);
            const int128_t prod = sa * sb;
            const auto uprod = static_cast<uint128_t>(prod);
            const std::uint64_t wmask = width_mask(width);
            const std::uint64_t lo = static_cast<std::uint64_t>(uprod) & wmask;
            const std::uint64_t hi = static_cast<std::uint64_t>(uprod >> width) & wmask;
            if (width == 8) {
                write_gpr_sized(state, Gpr::kRax, OperandSize::k16, false, (hi << 8) | lo);
            } else {
                write_gpr_sized(state, Gpr::kRax, insn.dst.size, false, lo);
                write_gpr_sized(state, Gpr::kRdx, insn.dst.size, false, hi);
            }
            const bool exact = (static_cast<int128_t>(sign_extend(lo, width)) == prod);
            state.flags.cf = !exact;
            state.flags.of = !exact;
            break;
        }
        // 2-operand (`dst *= src`) or 3-operand (`dst = src * aux`)
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        if (insn.has_aux) {
            if (!read_operand(insn.src, state, memory, tlb, next_rip, a) ||
                !read_operand(insn.aux, state, memory, tlb, next_rip, b)) {
                return fault("imul: operand fault");
            }
        } else {
            if (!read_operand(insn.dst, state, memory, tlb, next_rip, a) ||
                !read_operand(insn.src, state, memory, tlb, next_rip, b)) {
                return fault("imul: operand fault");
            }
        }
        const int128_t prod = static_cast<int128_t>(sign_extend(a, width)) *
                              static_cast<int128_t>(sign_extend(b, width));
        const std::uint64_t res =
            static_cast<std::uint64_t>(static_cast<uint128_t>(prod)) & width_mask(width);
        const bool exact = (static_cast<int128_t>(sign_extend(res, width)) == prod);
        state.flags.cf = !exact;
        state.flags.of = !exact;
        if (!write_operand(insn.dst, state, memory, tlb, next_rip, res)) {
            return fault("imul: write fault");
        }
        break;
    }

    case Opcode::kDiv: {
        std::uint64_t d = 0;
        if (!read_operand(insn.dst, state, memory, tlb, next_rip, d)) {
            return fault("div: operand fault");
        }
        const std::uint64_t wmask = width_mask(width);
        const std::uint64_t denom = d & wmask;
        if (denom == 0) {
            return fault("div: divide by zero");
        }
        uint128_t numer = 0;
        if (width == 8) {
            numer = state.reg(Gpr::kRax) & 0xFFFFULL;
        } else {
            numer = (static_cast<uint128_t>(state.reg(Gpr::kRdx) & wmask) << width) |
                    static_cast<uint128_t>(state.reg(Gpr::kRax) & wmask);
        }
        const uint128_t q = numer / denom;
        const uint128_t rem = numer % denom;
        if (q > wmask) {
            return fault("div: quotient overflow");
        }
        if (width == 8) {
            write_gpr_sized(state, Gpr::kRax, OperandSize::k16, false,
                            (static_cast<std::uint64_t>(rem) << 8) | static_cast<std::uint64_t>(q));
        } else {
            write_gpr_sized(state, Gpr::kRax, insn.dst.size, false, static_cast<std::uint64_t>(q));
            write_gpr_sized(state, Gpr::kRdx, insn.dst.size, false,
                            static_cast<std::uint64_t>(rem));
        }
        break;
    }

    case Opcode::kIdiv: {
        std::uint64_t d = 0;
        if (!read_operand(insn.dst, state, memory, tlb, next_rip, d)) {
            return fault("idiv: operand fault");
        }
        const int128_t denom = sign_extend(d, width);
        if (denom == 0) {
            return fault("idiv: divide by zero");
        }
        int128_t numer = 0;
        if (width == 8) {
            numer = sign_extend(state.reg(Gpr::kRax) & 0xFFFFULL, 16);
        } else {
            const std::uint64_t wmask = width_mask(width);
            const uint128_t u = (static_cast<uint128_t>(state.reg(Gpr::kRdx) & wmask) << width) |
                                static_cast<uint128_t>(state.reg(Gpr::kRax) & wmask);
            if (width == 64) {
                numer = static_cast<int128_t>(u);
            } else {
                const int sh = 128 - 2 * width;
                numer = static_cast<int128_t>(u << sh) >> sh;
            }
        }
        const int128_t min_q = -(static_cast<int128_t>(1) << (width - 1));
        const int128_t max_q = (static_cast<int128_t>(1) << (width - 1)) - 1;
        if (numer == (min_q << width) && denom == -1) {
            return fault("idiv: signed overflow");
        }
        const int128_t q = numer / denom;
        const int128_t rem = numer % denom;
        if (q < min_q || q > max_q) {
            return fault("idiv: quotient overflow");
        }
        if (width == 8) {
            const std::uint64_t packed = ((static_cast<std::uint64_t>(rem) & 0xFFULL) << 8) |
                                         (static_cast<std::uint64_t>(q) & 0xFFULL);
            write_gpr_sized(state, Gpr::kRax, OperandSize::k16, false, packed);
        } else {
            write_gpr_sized(state, Gpr::kRax, insn.dst.size, false, static_cast<std::uint64_t>(q));
            write_gpr_sized(state, Gpr::kRdx, insn.dst.size, false,
                            static_cast<std::uint64_t>(rem));
        }
        break;
    }

    case Opcode::kSetcc: {
        const std::uint64_t v = alu::evaluate(insn.condition, state.flags) ? 1ULL : 0ULL;
        if (!write_operand(insn.dst, state, memory, tlb, next_rip, v)) {
            return fault("setcc: write fault");
        }
        break;
    }

    case Opcode::kCmovcc: {
        std::uint64_t src_val = 0;
        if (!read_operand(insn.src, state, memory, tlb, next_rip, src_val)) {
            return fault("cmovcc: memory fault");
        }
        if (alu::evaluate(insn.condition, state.flags)) {
            write_gpr_sized(state, insn.dst.reg, insn.dst.size, false, src_val);
        } else if (insn.dst.size == OperandSize::k32) {
            // Architectural x86-64 rule: 32-bit CMOVcc zero-extends upper 32 bits even when false.
            state.reg(insn.dst.reg) &= 0xFFFFFFFFULL;
        }
        break;
    }

    case Opcode::kXchg: {
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        if (!read_operand(insn.dst, state, memory, tlb, next_rip, a) ||
            !read_operand(insn.src, state, memory, tlb, next_rip, b)) {
            return fault("xchg: memory fault");
        }
        if (!write_operand(insn.dst, state, memory, tlb, next_rip, b) ||
            !write_operand(insn.src, state, memory, tlb, next_rip, a)) {
            return fault("xchg: write fault");
        }
        break;
    }

    case Opcode::kCmpxchg: {
        std::uint64_t dst_val = 0;
        std::uint64_t src_val = 0;
        if (!read_operand(insn.dst, state, memory, tlb, next_rip, dst_val) ||
            !read_operand(insn.src, state, memory, tlb, next_rip, src_val)) {
            return fault("cmpxchg: memory fault");
        }
        const std::uint64_t acc = read_gpr_sized(state, Gpr::kRax, insn.dst.size, false);
        (void)alu::sub(acc, dst_val, width, state.flags);
        if ((acc & width_mask(width)) == (dst_val & width_mask(width))) {
            if (!write_operand(insn.dst, state, memory, tlb, next_rip, src_val)) {
                return fault("cmpxchg: write fault");
            }
        } else {
            write_gpr_sized(state, Gpr::kRax, insn.dst.size, false, dst_val);
        }
        break;
    }

    case Opcode::kXadd: {
        std::uint64_t dst_val = 0;
        std::uint64_t src_val = 0;
        if (!read_operand(insn.dst, state, memory, tlb, next_rip, dst_val) ||
            !read_operand(insn.src, state, memory, tlb, next_rip, src_val)) {
            return fault("xadd: memory fault");
        }
        const std::uint64_t sum = alu::add(dst_val, src_val, width, state.flags);
        if (!write_operand(insn.src, state, memory, tlb, next_rip, dst_val) ||
            !write_operand(insn.dst, state, memory, tlb, next_rip, sum)) {
            return fault("xadd: write fault");
        }
        break;
    }

    case Opcode::kBswap: {
        if (width == 64) {
            state.reg(insn.dst.reg) = __builtin_bswap64(state.reg(insn.dst.reg));
        } else {
            const auto v32 = static_cast<std::uint32_t>(state.reg(insn.dst.reg));
            state.reg(insn.dst.reg) = __builtin_bswap32(v32);
        }
        break;
    }

    case Opcode::kBsf:
    case Opcode::kBsr: {
        std::uint64_t v = 0;
        if (!read_operand(insn.src, state, memory, tlb, next_rip, v)) {
            return fault("bsf/bsr: memory fault");
        }
        const std::uint64_t masked = v & width_mask(width);
        if (insn.rep == 0xF3) {
            // TZCNT / LZCNT
            if (masked == 0) {
                state.flags.cf = true;
                state.flags.zf = false;
                write_gpr_sized(state, insn.dst.reg, insn.dst.size, false,
                                static_cast<std::uint64_t>(width));
            } else {
                const unsigned idx =
                    (insn.opcode == Opcode::kBsf)
                        ? static_cast<unsigned>(std::countr_zero(masked))
                        : static_cast<unsigned>(width - 1 - (63 - std::countl_zero(masked)));
                state.flags.cf = false;
                state.flags.zf = (idx == 0);
                write_gpr_sized(state, insn.dst.reg, insn.dst.size, false, idx);
            }
        } else if (masked == 0) {
            state.flags.zf = true;
        } else {
            state.flags.zf = false;
            const unsigned idx = (insn.opcode == Opcode::kBsf)
                                     ? static_cast<unsigned>(std::countr_zero(masked))
                                     : static_cast<unsigned>(63 - std::countl_zero(masked));
            write_gpr_sized(state, insn.dst.reg, insn.dst.size, false, idx);
        }
        break;
    }

    case Opcode::kBt:
    case Opcode::kBts:
    case Opcode::kBtr:
    case Opcode::kBtc: {
        std::uint64_t bit_raw = 0;
        if (!read_operand(insn.src, state, memory, tlb, next_rip, bit_raw)) {
            return fault("bt: source fault");
        }
        if (insn.dst.kind == OperandKind::kMemory && insn.src.kind == OperandKind::kRegister) {
            // Register bit offset into memory adds signed word offset `(bit >> log2(width)) *
            // bytes`!
            const std::int64_t signed_bit = sign_extend(bit_raw, width);
            const std::int64_t word_idx = signed_bit >> (width == 64 ? 6 : (width == 32 ? 5 : 4));
            const auto bit_in_word = static_cast<unsigned>(signed_bit & (width - 1));
            const std::uint64_t addr = effective_address(insn.dst.mem, state, next_rip) +
                                       static_cast<std::uint64_t>(word_idx * (width / 8));
            std::uint64_t word = 0;
            const auto bytes = static_cast<std::size_t>(width / 8);
            if (!fast_mem_read(memory, tlb, addr, &word, bytes)) {
                return fault("bt: memory fault");
            }
            const std::uint64_t bit_mask = 1ULL << bit_in_word;
            state.flags.cf = (word & bit_mask) != 0;
            if (insn.opcode != Opcode::kBt) {
                if (insn.opcode == Opcode::kBts) {
                    word |= bit_mask;
                } else if (insn.opcode == Opcode::kBtr) {
                    word &= ~bit_mask;
                } else {
                    word ^= bit_mask;
                }
                if (!fast_mem_write(memory, tlb, addr, &word, bytes)) {
                    return fault("bt: write fault");
                }
            }
        } else {
            std::uint64_t word = 0;
            if (!read_operand(insn.dst, state, memory, tlb, next_rip, word)) {
                return fault("bt: operand fault");
            }
            const auto bit_in_word =
                static_cast<unsigned>(bit_raw & static_cast<std::uint64_t>(width - 1));
            const std::uint64_t bit_mask = 1ULL << bit_in_word;
            state.flags.cf = (word & bit_mask) != 0;
            if (insn.opcode != Opcode::kBt) {
                if (insn.opcode == Opcode::kBts) {
                    word |= bit_mask;
                } else if (insn.opcode == Opcode::kBtr) {
                    word &= ~bit_mask;
                } else {
                    word ^= bit_mask;
                }
                if (!write_operand(insn.dst, state, memory, tlb, next_rip, word)) {
                    return fault("bt: write fault");
                }
            }
        }
        break;
    }

    case Opcode::kJmp: {
        std::uint64_t target = 0;
        if (!read_operand(insn.dst, state, memory, tlb, next_rip, target)) {
            return fault("jmp: indirect target fault");
        }
        state.rip = target;
        result.status = StepStatus::kOk;
        return result;
    }

    case Opcode::kJcc:
        state.rip = alu::evaluate(insn.condition, state.flags) ? insn.dst.imm : next_rip;
        return result;

    case Opcode::kCall: {
        std::uint64_t target = 0;
        if (!read_operand(insn.dst, state, memory, tlb, next_rip, target)) {
            return fault("call: target memory fault");
        }
        const std::uint64_t new_rsp = state.reg(Gpr::kRsp) - 8;
        if (!fast_mem_write(memory, tlb, new_rsp, &next_rip, 8)) {
            return fault("call: stack fault");
        }
        state.reg(Gpr::kRsp) = new_rsp;
        state.rip = target;
        result.status = StepStatus::kOk;
        return result;
    }

    case Opcode::kRet: {
        const std::uint64_t rsp = state.reg(Gpr::kRsp);
        if (rsp == 0) {
            result.status = StepStatus::kHalted;
            result.message = "ret (top-level return)";
            return result;
        }
        std::uint64_t ret_addr = 0;
        if (!fast_mem_read(memory, tlb, rsp, &ret_addr, 8)) {
            return fault("ret: stack fault");
        }
        if (ret_addr == 0) {
            // Zero return sentinel on stack: clean top-level return (preserves
            // standalone function-call tests and bare snippet harnesses).
            result.status = StepStatus::kHalted;
            result.message = "ret (top-level return)";
            return result;
        }
        state.reg(Gpr::kRsp) = rsp + 8 + insn.dst.imm;
        state.rip = ret_addr;
        result.status = StepStatus::kOk;
        return result;
    }

    case Opcode::kLeave: {
        const std::uint64_t old_rbp = state.reg(Gpr::kRbp);
        std::uint64_t new_rbp = 0;
        if (!fast_mem_read(memory, tlb, old_rbp, &new_rbp, 8)) {
            return fault("leave: stack fault");
        }
        state.reg(Gpr::kRsp) = old_rbp + 8;
        state.reg(Gpr::kRbp) = new_rbp;
        break;
    }

    case Opcode::kPush: {
        std::uint64_t value = 0;
        if (!read_operand(insn.dst, state, memory, tlb, next_rip, value)) {
            return fault("push: operand fault");
        }
        const std::uint64_t rsp = state.reg(Gpr::kRsp) - 8;
        if (!fast_mem_write(memory, tlb, rsp, &value, 8)) {
            return fault("push: stack fault");
        }
        state.reg(Gpr::kRsp) = rsp;
        break;
    }

    case Opcode::kPop: {
        const std::uint64_t rsp = state.reg(Gpr::kRsp);
        std::uint64_t value = 0;
        if (!fast_mem_read(memory, tlb, rsp, &value, 8)) {
            return fault("pop: stack fault");
        }
        state.reg(Gpr::kRsp) = rsp + 8;
        if (insn.dst.kind == OperandKind::kRegister) {
            state.reg(insn.dst.reg) = value;
        } else if (!write_operand(insn.dst, state, memory, tlb, next_rip, value)) {
            state.reg(Gpr::kRsp) = rsp;
            return fault("pop: destination memory fault");
        }
        break;
    }
    }

    state.rip = next_rip;
    result.status = StepStatus::kOk;
    return result;
}

} // namespace

bool soft_tlb_read_slow(Memory& memory, SoftTlb& tlb, std::uint64_t addr, void* out,
                        std::size_t bytes) noexcept {
    return fast_mem_read(memory, &tlb, addr, out, bytes);
}

bool soft_tlb_write_slow(Memory& memory, SoftTlb& tlb, std::uint64_t addr, const void* in,
                         std::size_t bytes) noexcept {
    return fast_mem_write(memory, &tlb, addr, in, bytes);
}

bool execute_cached_helper(const Instruction& insn, GuestState& state, Memory& memory,
                           SoftTlb* tlb) {
    const FastStepOutcome r = execute_decoded(state, memory, tlb, insn, /*syscalls=*/nullptr);
    return r.status == StepStatus::kOk;
}

StepResult step(GuestState& state, Memory& memory, SyscallHandler* syscalls) {
    StepResult result;

    std::uint8_t code[15] = {};
    std::size_t available = 0;
    for (std::size_t n = sizeof(code); n >= 1; --n) {
        if (memory.fetch(state.rip, code, n)) {
            available = n;
            break;
        }
    }
    if (available == 0) {
        result.status = StepStatus::kFault;
        result.message = "instruction fetch fault at rip";
        return result;
    }

    const DecodeResult decoded = decode(code, available, state.rip);
    result.instruction = decoded.instruction;
    if (!decoded.ok()) {
        result.status = StepStatus::kUnsupported;
        result.decode_error = decoded.error;
        result.message = to_string(decoded.error);
        return result;
    }

    const FastStepOutcome fast =
        execute_decoded(state, memory, /*tlb=*/nullptr, decoded.instruction, syscalls);
    result.status = fast.status;
    result.unimplemented_syscalls = fast.unimplemented_syscalls;
    result.exited_via_syscall = fast.exited_via_syscall;
    result.exit_code = fast.exit_code;
    if (fast.message != nullptr) {
        result.message = fast.message;
    }
    return result;
}

RunResult run_program(Memory& memory, GuestState state, std::uint64_t max_steps,
                      SyscallHandler* syscalls) {
    RunResult out;
    out.state = state;

    struct DecodedSlot {
        std::uint64_t pc = 0;
        std::uint64_t epoch = 0;
        Instruction insn{};
    };
    constexpr std::size_t kDecodeCacheSize = 8192;
    constexpr std::size_t kDecodeCacheMask = kDecodeCacheSize - 1;
    std::vector<DecodedSlot> icache(kDecodeCacheSize);

    SoftTlb tlb;
    tlb.flush(memory.code_epoch());

    for (std::uint64_t i = 0; i < max_steps; ++i) {
        const std::uint64_t pc = out.state.rip;
        const std::uint64_t epoch = memory.code_epoch();
        if (epoch != tlb.epoch) {
            tlb.flush(epoch);
        }
        DecodedSlot& slot = icache[pc & kDecodeCacheMask];
        if (epoch == 0 || slot.epoch != epoch || slot.pc != pc) {
            std::uint8_t code[15] = {};
            std::size_t available = 0;
            for (std::size_t n = sizeof(code); n >= 1; --n) {
                if (memory.fetch(pc, code, n)) {
                    available = n;
                    break;
                }
            }
            if (available == 0) {
                out.status = RunStatus::kFault;
                out.message = "instruction fetch fault at rip";
                return out;
            }
            const DecodeResult decoded = decode(code, available, pc);
            if (!decoded.ok()) {
                out.status = RunStatus::kUnsupported;
                out.message = to_string(decoded.error);
                return out;
            }
            slot.pc = pc;
            slot.epoch = epoch;
            slot.insn = decoded.instruction;
        }

        const FastStepOutcome r = execute_decoded(out.state, memory, &tlb, slot.insn, syscalls);
        out.unimplemented_syscalls += r.unimplemented_syscalls;
        switch (r.status) {
        case StepStatus::kOk:
            ++out.steps;
            break;
        case StepStatus::kHalted:
            ++out.steps;
            out.status = RunStatus::kOk;
            out.exited_via_syscall = r.exited_via_syscall;
            out.exit_code = r.exit_code;
            if (r.message != nullptr) {
                out.message = r.message;
            }
            return out;
        case StepStatus::kFault:
            out.status = RunStatus::kFault;
            if (r.message != nullptr) {
                out.message = r.message;
            }
            return out;
        case StepStatus::kUnsupported:
            out.status = RunStatus::kUnsupported;
            if (r.message != nullptr) {
                out.message = r.message;
            }
            return out;
        }
    }
    out.status = RunStatus::kStepLimit;
    out.message = "step limit reached without halting";
    return out;
}

} // namespace hyperion
