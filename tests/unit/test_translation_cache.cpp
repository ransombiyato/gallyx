// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Translation-cache tests.
//
// The cache is the component that decides *whether* it is safe to execute
// generated code at all. That decision is tested everywhere: on a host that
// cannot execute its own generated code, run() must refuse and execute nothing,
// rather than pretend. The differential test that generated blocks agree with
// the interpreter lives in test_translator.cpp, where the rest of the
// translation pipeline is covered.
#include <cstdint>
#include <cstring>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/exec_mem.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/translation_cache.hpp"

using hyperion::ExecMem;
using hyperion::FlatMemory;
using hyperion::Gpr;
using hyperion::GuestState;
using hyperion::RunStatus;
using hyperion::TranslationCache;

namespace {
constexpr std::uint64_t kBase = 0x400000;
constexpr std::size_t kMemSize = 4096;
} // namespace

HYPERION_TEST("translation_cache", "refuses_to_run_without_exec_support") {
    if (ExecMem::executable_supported()) {
        HYPERION_SKIP("host can execute generated code; refusal path not exercised");
    }
    // mov eax, 1 ; hlt
    const std::vector<std::uint8_t> program = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xF4};
    FlatMemory memory(kBase, kMemSize);
    std::memcpy(memory.data(), program.data(), program.size());

    GuestState initial;
    initial.rip = kBase;
    TranslationCache cache;
    const hyperion::RunResult r = cache.run(memory, initial);

    HYPERION_CHECK(r.status == RunStatus::kUnsupported);
    HYPERION_CHECK_EQ(r.steps, std::uint64_t{0});
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRax), std::uint64_t{0}); // nothing ran
    HYPERION_CHECK_EQ(r.state.rip, kBase);
    HYPERION_CHECK_EQ(cache.block_count(), std::size_t{0}); // nothing compiled
}

#if defined(__aarch64__)

HYPERION_TEST("translation_cache", "caches_blocks_and_agrees_with_interpreter") {
    // A loop body: mov ecx, 5 ; add rbx, rax ; dec rcx ; jnz -5 ; hlt
    const std::vector<std::uint8_t> program = {
        0xB9, 0x05, 0x00, 0x00, 0x00, 0x48, 0x01, 0xC3, 0x48, 0xFF, 0xC9, 0x75, 0xF8, 0xF4,
    };

    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRax) = 3;

    FlatMemory interp_mem(kBase, kMemSize);
    std::memcpy(interp_mem.data(), program.data(), program.size());
    const hyperion::RunResult expected = hyperion::run_program(interp_mem, initial);

    FlatMemory cache_mem(kBase, kMemSize);
    std::memcpy(cache_mem.data(), program.data(), program.size());
    TranslationCache cache;
    const hyperion::RunResult actual = cache.run(cache_mem, initial);

    HYPERION_CHECK(actual.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(actual.steps, expected.steps);
    HYPERION_CHECK_EQ(actual.state.reg(Gpr::kRbx), expected.state.reg(Gpr::kRbx));
    HYPERION_CHECK_EQ(actual.state.rip, expected.state.rip);
    // The loop is entered repeatedly but translated once per distinct PC, so the
    // block count stays small: this is the cache working, not a re-translation.
    HYPERION_CHECK(cache.block_count() > 0);
    HYPERION_CHECK(cache.block_count() <= 4);
}

#else

HYPERION_TEST("translation_cache", "caches_blocks_and_agrees_with_interpreter") {
    HYPERION_SKIP("generated code is AArch64; requires an AArch64 host to run");
}

#endif
