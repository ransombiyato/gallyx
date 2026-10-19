// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest-state unit tests. The register file and RFLAGS encoding are the
// contract between the interpreter and the future code generator, so the
// packing rules are pinned down here.
#include <cstdint>

#include "harness/test_harness.hpp"
#include "hyperion/guest_state.hpp"

using hyperion::Flags;
using hyperion::Gpr;
using hyperion::GuestState;

HYPERION_TEST("guest_state", "gpr_indices_match_x86_encoding_order") {
    // The enum values must equal the register numbers used by ModRM/REX.
    HYPERION_CHECK_EQ(static_cast<int>(Gpr::kRax), 0);
    HYPERION_CHECK_EQ(static_cast<int>(Gpr::kRcx), 1);
    HYPERION_CHECK_EQ(static_cast<int>(Gpr::kRdx), 2);
    HYPERION_CHECK_EQ(static_cast<int>(Gpr::kRbx), 3);
    HYPERION_CHECK_EQ(static_cast<int>(Gpr::kRsp), 4);
    HYPERION_CHECK_EQ(static_cast<int>(Gpr::kRbp), 5);
    HYPERION_CHECK_EQ(static_cast<int>(Gpr::kRsi), 6);
    HYPERION_CHECK_EQ(static_cast<int>(Gpr::kRdi), 7);
    HYPERION_CHECK_EQ(static_cast<int>(Gpr::kR8), 8);
    HYPERION_CHECK_EQ(static_cast<int>(Gpr::kR15), 15);
}

HYPERION_TEST("guest_state", "fresh_state_is_zeroed") {
    const GuestState s;
    for (int i = 0; i < 16; ++i) {
        HYPERION_CHECK_EQ(s.gpr[i], 0ULL);
    }
    HYPERION_CHECK_EQ(s.rip, 0ULL);
    HYPERION_CHECK(!s.flags.cf && !s.flags.zf && !s.flags.sf && !s.flags.of);
}

HYPERION_TEST("guest_state", "reg_accessor_matches_array") {
    GuestState s;
    s.reg(Gpr::kR10) = 0x1234;
    HYPERION_CHECK_EQ(s.gpr[10], 0x1234ULL);
    const GuestState& cs = s;
    HYPERION_CHECK_EQ(cs.reg(Gpr::kR10), 0x1234ULL);
}

HYPERION_TEST("guest_state", "rflags_encoding_bits") {
    // Verify each modelled flag maps to its architectural RFLAGS bit.
    Flags f;
    f.cf = true;
    HYPERION_CHECK_EQ(hyperion::pack_flags(f) & 0x1ULL, 0x1ULL);
    f = Flags{};
    f.pf = true;
    HYPERION_CHECK_EQ(hyperion::pack_flags(f) & 0x4ULL, 0x4ULL);
    f = Flags{};
    f.af = true;
    HYPERION_CHECK_EQ(hyperion::pack_flags(f) & 0x10ULL, 0x10ULL);
    f = Flags{};
    f.zf = true;
    HYPERION_CHECK_EQ(hyperion::pack_flags(f) & 0x40ULL, 0x40ULL);
    f = Flags{};
    f.sf = true;
    HYPERION_CHECK_EQ(hyperion::pack_flags(f) & 0x80ULL, 0x80ULL);
    f = Flags{};
    f.of = true;
    HYPERION_CHECK_EQ(hyperion::pack_flags(f) & 0x800ULL, 0x800ULL);
}

HYPERION_TEST("guest_state", "rflags_bit1_always_set") {
    const Flags f;
    HYPERION_CHECK_EQ(hyperion::pack_flags(f), 0x2ULL);
}

HYPERION_TEST("guest_state", "unpack_ignores_unmodelled_bits") {
    // A real RFLAGS value with IF (bit 9) and DF (bit 10) set, plus the six
    // modelled flags. Unpacking must look at only the six.
    const std::uint64_t rflags = 0x202ULL | 0x400ULL | 0x1ULL; // IF, DF, CF
    const Flags f = hyperion::unpack_flags(rflags);
    HYPERION_CHECK(f.cf);
    HYPERION_CHECK(!f.zf);
    HYPERION_CHECK(!f.sf);
    // Round-tripping drops IF/DF, which is expected and intentional.
    HYPERION_CHECK_EQ(hyperion::pack_flags(f), 0x2ULL | 0x1ULL);
}

HYPERION_TEST("guest_state", "to_string_renders_flags") {
    // Order is C P A Z S O, matching to_string(const Flags&).
    Flags f;
    f.cf = true;
    f.zf = true;
    HYPERION_CHECK_EQ(hyperion::to_string(f), std::string("C--Z--"));
}
