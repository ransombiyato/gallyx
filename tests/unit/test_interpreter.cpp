// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Interpreter unit tests that do not depend on the host being x86-64: memory
// addressing, control flow, stack operations, and the parts of the flag model
// that can be checked from first principles. The hardware differential lives
// in tests/differential/.
#include <cstdint>
#include <cstring>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"

using hyperion::Flags;
using hyperion::FlatMemory;
using hyperion::Gpr;
using hyperion::GuestState;
using hyperion::StepStatus;

namespace {

constexpr std::uint64_t kBase = 0x400000;

GuestState run(const std::vector<std::uint8_t>& code, std::uint64_t a = 0, std::uint64_t b = 0,
               int max_steps = 32) {
    FlatMemory memory(kBase, 4096);
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.reg(Gpr::kRax) = a;
    state.reg(Gpr::kRbx) = b;
    state.reg(Gpr::kRsp) = kBase + 2048;
    state.rip = kBase;
    for (int i = 0; i < max_steps; ++i) {
        const auto r = hyperion::step(state, memory);
        if (r.status == StepStatus::kHalted) {
            break;
        }
        HYPERION_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(StepStatus::kOk));
        if (r.status != StepStatus::kOk) {
            break;
        }
    }
    return state;
}

} // namespace

HYPERION_TEST("interpreter", "mov_immediate_then_halt") {
    // mov rax, 0x1234 ; hlt
    const GuestState s = run({0x48, 0xB8, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF4});
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), 0x1234ULL);
}

HYPERION_TEST("interpreter", "rip_advances_by_instruction_length") {
    // nop ; hlt
    const GuestState s = run({0x90, 0xF4});
    HYPERION_CHECK_EQ(s.rip, kBase + 1);
}

HYPERION_TEST("interpreter", "store_and_load_through_memory") {
    // mov rbx, [rax] ; mov [rax+8], rbx ; hlt   with memory seeded below.
    FlatMemory memory(kBase, 4096);
    const std::vector<std::uint8_t> code = {0x48, 0x8B, 0x18,       // mov rbx, [rax]
                                            0x48, 0x89, 0x58, 0x08, // mov [rax+8], rbx
                                            0xF4};
    std::memcpy(memory.data(), code.data(), code.size());
    const std::uint64_t data = kBase + 0x100;
    const std::uint64_t value = 0xDEADBEEFCAFEF00DULL;
    memory.write(data, reinterpret_cast<const std::uint8_t*>(&value), 8);

    GuestState state;
    state.reg(Gpr::kRax) = data;
    state.rip = kBase;
    for (int i = 0; i < 8; ++i) {
        const auto r = hyperion::step(state, memory);
        if (r.status == StepStatus::kHalted) {
            break;
        }
        HYPERION_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(StepStatus::kOk));
    }
    HYPERION_CHECK_EQ(state.reg(Gpr::kRbx), value);
    std::uint64_t round_trip = 0;
    memory.read(data + 8, reinterpret_cast<std::uint8_t*>(&round_trip), 8);
    HYPERION_CHECK_EQ(round_trip, value);
}

HYPERION_TEST("interpreter", "sib_addressing") {
    // mov rax, [rbx + rcx*4 + 0x10] ; hlt
    // Encoding: 48 8B 44 8B 10
    //   44 = mod=01 (disp8), reg=000 (rax), rm=100 (SIB follows)
    //   8B = scale=4, index=001 (rcx), base=011 (rbx)
    //   10 = disp8 = 0x10
    FlatMemory memory(kBase, 4096);
    const std::vector<std::uint8_t> code = {0x48, 0x8B, 0x44, 0x8B, 0x10, 0xF4};
    std::memcpy(memory.data(), code.data(), code.size());
    const std::uint64_t target = kBase + 0x200;
    const std::uint64_t value = 0x1122334455667788ULL;
    memory.write(target, reinterpret_cast<const std::uint8_t*>(&value), 8);

    GuestState state;
    state.reg(Gpr::kRbx) = kBase + 0x100;
    state.reg(Gpr::kRcx) = 0x3C; // 0x100 + 0x3C*4 + 0x10 = 0x200 -> target
    state.rip = kBase;
    for (int i = 0; i < 4; ++i) {
        const auto r = hyperion::step(state, memory);
        if (r.status == StepStatus::kHalted) {
            break;
        }
        HYPERION_CHECK_EQ(static_cast<int>(r.status), static_cast<int>(StepStatus::kOk));
    }
    HYPERION_CHECK_EQ(state.reg(Gpr::kRax), value);
}

HYPERION_TEST("interpreter", "conditional_branch_taken_and_not_taken") {
    // cmp rax, rbx ; je +5 ; mov ebx, 1 ; hlt
    // The 0xC7 (mov r/m, imm32) encoding is not in the subset, so the body
    // uses the supported MOV r32, imm32 form (0xBB = B8+rbx).
    const std::vector<std::uint8_t> code = {0x48, 0x39, 0xD8,             // cmp rax, rbx
                                            0x74, 0x05,                   // je +5
                                            0xBB, 0x01, 0x00, 0x00, 0x00, // mov ebx, 1
                                            0xF4};                        // hlt

    const GuestState equal = run(code, 5, 5);
    HYPERION_CHECK_EQ(equal.reg(Gpr::kRbx), 5ULL); // jump taken, not overwritten
    HYPERION_CHECK(equal.flags.zf);

    const GuestState unequal = run(code, 5, 6);
    HYPERION_CHECK_EQ(unequal.reg(Gpr::kRbx), 1ULL); // jump not taken, mov ran
    HYPERION_CHECK(!unequal.flags.zf);
}

HYPERION_TEST("interpreter", "push_and_pop") {
    // mov eax, 0x42 ; push rax ; pop rbx ; hlt
    const GuestState s = run({0xB8, 0x42, 0x00, 0x00, 0x00, 0x50, 0x5B, 0xF4});
    HYPERION_CHECK_EQ(s.reg(Gpr::kRbx), 0x42ULL);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRsp), kBase + 2048); // balanced
}

HYPERION_TEST("interpreter", "unsupported_instruction_leaves_state_unchanged") {
    // A UD2 (0x0F 0x0B) is not in the subset. The interpreter must stop with
    // kUnsupported and must NOT advance rip or modify anything.
    FlatMemory memory(kBase, 4096);
    const std::vector<std::uint8_t> code = {0x0F, 0x0B};
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.reg(Gpr::kRax) = 0xABCD;
    state.rip = kBase;
    const auto r = hyperion::step(state, memory);
    HYPERION_CHECK(r.status == StepStatus::kUnsupported);
    HYPERION_CHECK_EQ(state.rip, kBase);
    HYPERION_CHECK_EQ(state.reg(Gpr::kRax), 0xABCDULL);
}

HYPERION_TEST("interpreter", "memory_fault_is_reported") {
    // mov rax, [rbx] with rbx pointing outside the flat region must fault, not
    // read zeros.
    FlatMemory memory(kBase, 4096);
    const std::vector<std::uint8_t> code = {0x48, 0x8B, 0x03};
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.reg(Gpr::kRbx) = 0xDEAD0000ULL;
    state.rip = kBase;
    const auto r = hyperion::step(state, memory);
    HYPERION_CHECK(r.status == StepStatus::kFault);
}

HYPERION_TEST("interpreter", "inc_preserves_carry_flag") {
    // inc rax with CF set must leave CF set but update ZF.
    FlatMemory memory(kBase, 4096);
    const std::vector<std::uint8_t> code = {0x48, 0xFF, 0xC0};
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.reg(Gpr::kRax) = 0xFFFFFFFFFFFFFFFFULL; // +1 -> 0, ZF set
    state.flags.cf = true;
    state.rip = kBase;
    const auto r = hyperion::step(state, memory);
    HYPERION_CHECK(r.status == StepStatus::kOk);
    HYPERION_CHECK_EQ(state.reg(Gpr::kRax), 0ULL);
    HYPERION_CHECK(state.flags.zf);
    HYPERION_CHECK(state.flags.cf); // preserved
}

HYPERION_TEST("interpreter", "logical_op_clears_carry_and_overflow") {
    FlatMemory memory(kBase, 4096);
    const std::vector<std::uint8_t> code = {0x48, 0x31, 0xD8}; // xor rax, rbx
    std::memcpy(memory.data(), code.data(), code.size());
    GuestState state;
    state.reg(Gpr::kRax) = 0xF0F0;
    state.reg(Gpr::kRbx) = 0xF0F0; // xor -> 0
    state.flags.cf = true;
    state.flags.of = true;
    state.rip = kBase;
    const auto r = hyperion::step(state, memory);
    HYPERION_CHECK(r.status == StepStatus::kOk);
    HYPERION_CHECK_EQ(state.reg(Gpr::kRax), 0ULL);
    HYPERION_CHECK(state.flags.zf);
    HYPERION_CHECK(!state.flags.cf);
    HYPERION_CHECK(!state.flags.of);
}

HYPERION_TEST("interpreter", "flags_pack_unpack_round_trip") {
    Flags f;
    f.cf = true;
    f.pf = true;
    f.zf = true;
    const std::uint64_t packed = hyperion::pack_flags(f);
    const Flags back = hyperion::unpack_flags(packed);
    HYPERION_CHECK(back == f);
    // Bit 1 is architecturally set on real hardware.
    HYPERION_CHECK((packed & 0x2ULL) != 0);
}

HYPERION_TEST("interpreter", "condition_evaluation_table") {
    Flags f;
    f.zf = true;
    // With ZF set: E/NE, BE/A, LE/G must all behave.
    HYPERION_CHECK(hyperion::alu::evaluate(hyperion::Condition::kE, f));
    HYPERION_CHECK(!hyperion::alu::evaluate(hyperion::Condition::kNe, f));
    HYPERION_CHECK(hyperion::alu::evaluate(hyperion::Condition::kBe, f));
    HYPERION_CHECK(!hyperion::alu::evaluate(hyperion::Condition::kA, f));
    HYPERION_CHECK(hyperion::alu::evaluate(hyperion::Condition::kLe, f));
    HYPERION_CHECK(!hyperion::alu::evaluate(hyperion::Condition::kG, f));

    // Signed vs unsigned: SF != OF means "less than" for signed comparisons.
    Flags g;
    g.sf = true;
    g.of = false;
    HYPERION_CHECK(hyperion::alu::evaluate(hyperion::Condition::kL, g));
    HYPERION_CHECK(hyperion::alu::evaluate(hyperion::Condition::kB, g) == false);
}
