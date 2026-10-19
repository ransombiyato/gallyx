// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// AArch64 encoder tests. The expected words below are the exact output of
// GNU as / objdump (binutils 2.44), captured from this source:
//
//   movz x0, #0x1234
//   movz x1, #0xabcd, lsl #16
//   movk x0, #0xbeef, lsl #32
//   movz w2, #0x5678
//   add x3, x4, x5
//   sub x6, x7, x8
//   and x9, x10, x11
//   orr x12, x13, x14
//   eor x15, x16, x17
//   add w18, w19, w20
//   sub w21, w22, w23
//   add x0, x1, #0x1ff
//   sub x2, x3, #0x7
//   cmp x4, x5
//   cmp x6, #42
//   b 4 ; b.eq 8 ; b.ne -4
//   ret
//
// This is deliberate: an encoder checked only against itself proves nothing.
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/aarch64_encoder.hpp"

using hyperion::a64::Cond;
using hyperion::a64::Encoder;
using hyperion::a64::Width;

HYPERION_TEST("aarch64_encoder", "data_processing_register_matches_binutils") {
    Encoder e;
    e.add(Width::k64, 3, 4, 5);
    e.sub(Width::k64, 6, 7, 8);
    e.and_(Width::k64, 9, 10, 11);
    e.orr(Width::k64, 12, 13, 14);
    e.eor(Width::k64, 15, 16, 17);
    e.add(Width::k32, 18, 19, 20);
    e.sub(Width::k32, 21, 22, 23);

    const std::vector<std::uint32_t> expected = {
        0x8B050083u, // add x3, x4, x5
        0xCB0800E6u, // sub x6, x7, x8
        0x8A0B0149u, // and x9, x10, x11
        0xAA0E01ACu, // orr x12, x13, x14
        0xCA11020Fu, // eor x15, x16, x17
        0x0B140272u, // add w18, w19, w20
        0x4B1702D5u, // sub w21, w22, w23
    };
    HYPERION_CHECK(e.code() == expected);
}

HYPERION_TEST("aarch64_encoder", "moves_match_binutils") {
    Encoder e;
    e.movz(Width::k64, 0, 0x1234);
    e.movz(Width::k64, 1, 0xABCD, 16);
    e.movk(Width::k64, 0, 0xBEEF, 32);
    e.movz(Width::k32, 2, 0x5678);

    const std::vector<std::uint32_t> expected = {
        0xD2824680u, // movz x0, #0x1234
        0xD2B579A1u, // movz x1, #0xabcd, lsl #16
        0xF2D7DDE0u, // movk x0, #0xbeef, lsl #32
        0x528ACF02u, // movz w2, #0x5678
    };
    HYPERION_CHECK(e.code() == expected);
}

HYPERION_TEST("aarch64_encoder", "immediate_arith_and_cmp_match_binutils") {
    Encoder e;
    e.add_imm(Width::k64, 0, 1, 0x1FF);
    e.sub_imm(Width::k64, 2, 3, 7);
    e.cmp(Width::k64, 4, 5);
    e.cmp_imm(Width::k64, 6, 42);

    const std::vector<std::uint32_t> expected = {
        0x9107FC20u, // add x0, x1, #0x1ff
        0xD1001C62u, // sub x2, x3, #0x7
        0xEB05009Fu, // cmp x4, x5
        0xF100A8DFu, // cmp x6, #0x2a
    };
    HYPERION_CHECK(e.code() == expected);
}

HYPERION_TEST("aarch64_encoder", "ret_and_nop") {
    Encoder e;
    e.ret();
    e.nop();
    HYPERION_CHECK_EQ(e.code()[0], 0xD65F03C0u); // ret
    HYPERION_CHECK_EQ(e.code()[1], 0xD503201Fu); // nop
}

HYPERION_TEST("aarch64_encoder", "forward_branch_patch") {
    // b <skip> ; nop ; skip: nop ; nop
    // skip is at instruction index 2, so the branch spans 2 instructions.
    // binutils: `b 8` (forward 8 bytes) = 0x14000002.
    Encoder e;
    const auto skip = e.new_label();
    e.b(skip);
    e.nop();
    e.bind(skip);
    e.nop();
    HYPERION_CHECK_EQ(e.code()[0], 0x14000002u);
}

HYPERION_TEST("aarch64_encoder", "backward_conditional_branch_patch") {
    // loop: nop ; b.ne loop
    // b.ne to an address 4 bytes back => imm19 = -1 => 0x54FFFFE1.
    // This is exactly the encoding binutils produced for `b.ne -4`.
    Encoder e;
    const auto loop = e.new_label();
    e.bind(loop);
    e.nop();
    e.b_cond(Cond::kNe, loop);
    HYPERION_CHECK_EQ(e.code()[1], 0x54FFFFE1u);
}

HYPERION_TEST("aarch64_encoder", "forward_conditional_branch_patch") {
    // b.eq <skip> ; nop ; skip: nop
    // skip is at instruction index 2 => imm19 = 2 => 0x54000040, which is
    // exactly the encoding binutils produced for `b.eq 14` from address 0xc
    // (a forward span of two instructions).
    Encoder e;
    const auto skip = e.new_label();
    e.b_cond(Cond::kEq, skip);
    e.nop();
    e.bind(skip);
    e.nop();
    HYPERION_CHECK_EQ(e.code()[0], 0x54000040u);
}

HYPERION_TEST("aarch64_encoder", "mov_imm64_uses_minimal_sequence") {
    Encoder e;
    e.mov_imm64(0, 0x1234);
    HYPERION_CHECK_EQ(e.count(), 1ULL); // just movz
    HYPERION_CHECK_EQ(e.code()[0], 0xD2824680u);

    Encoder e2;
    e2.mov_imm64(0, 0xABCD00001234ULL);
    // bits 0..15 = 0x1234, bits 16..31 = 0, bits 32..47 = 0xabcd.
    // => movz x0, #0x1234 ; movk x0, #0xabcd, lsl #32
    // binutils: `movk x0, #0xabcd, lsl #32` = 0xF2D579A0.
    HYPERION_CHECK_EQ(e2.count(), 2ULL);
    HYPERION_CHECK_EQ(e2.code()[0], 0xD2824680u);
    HYPERION_CHECK_EQ(e2.code()[1], 0xF2D579A0u);
}

HYPERION_TEST("aarch64_encoder", "bytes_are_little_endian") {
    Encoder e;
    e.ret(); // 0xD65F03C0
    const auto b = e.bytes();
    HYPERION_CHECK_EQ(b.size(), 4ULL);
    HYPERION_CHECK_EQ(b[0], 0xC0u);
    HYPERION_CHECK_EQ(b[1], 0x03u);
    HYPERION_CHECK_EQ(b[2], 0x5Fu);
    HYPERION_CHECK_EQ(b[3], 0xD6u);
}

HYPERION_TEST("aarch64_encoder", "unbound_label_is_rejected") {
    Encoder e;
    const auto never = e.new_label();
    e.b(never);
    bool threw = false;
    try {
        (void)e.bytes();
    } catch (const std::logic_error&) {
        threw = true;
    }
    HYPERION_CHECK(threw);
}

HYPERION_TEST("aarch64_encoder", "double_bind_is_rejected") {
    Encoder e;
    const auto l = e.new_label();
    e.bind(l);
    bool threw = false;
    try {
        e.bind(l);
    } catch (const std::logic_error&) {
        threw = true;
    }
    HYPERION_CHECK(threw);
}

HYPERION_TEST("aarch64_encoder", "imm12_overflow_is_rejected") {
    Encoder e;
    bool threw = false;
    try {
        e.add_imm(Width::k64, 0, 1, 0x1000); // one past the 12-bit range
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    HYPERION_CHECK(threw);
}
