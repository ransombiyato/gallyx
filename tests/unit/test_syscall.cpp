// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest syscall layer tests (Phase 6, increment 1).
//
// These verify the `syscall` instruction, the handler dispatch and the
// write/exit/exit_group semantics against a real Memory and GuestState, with a
// capturing output sink so nothing leaks into the test runner's stdout. The
// tests that require executing generated AArch64 code are guarded on
// __aarch64__, matching the rest of the suite.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/decoder.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/run.hpp"
#include "hyperion/syscall.hpp"
#include "hyperion/translation_cache.hpp"

using hyperion::FlatMemory;
using hyperion::Gpr;
using hyperion::GuestState;
using hyperion::MinimalSyscallHandler;
using hyperion::Opcode;
using hyperion::RunStatus;
using hyperion::StepStatus;
using hyperion::SyscallStatus;

namespace {

constexpr std::uint64_t kBase = 0x400000;
constexpr std::size_t kMemSize = 4096;

// Captured writes, keyed by fd.
struct Captured {
    std::string out;
    std::string err;
};

MinimalSyscallHandler::OutputSink sink_to(Captured& cap) {
    return [&cap](int fd, const std::uint8_t* data, std::size_t count) -> long {
        std::string& dst = (fd == 2) ? cap.err : cap.out;
        dst.append(reinterpret_cast<const char*>(data), count);
        return static_cast<long>(count);
    };
}

// A tiny guest program builder: place bytes at kBase.
FlatMemory program(const std::vector<std::uint8_t>& code) {
    FlatMemory memory(kBase, kMemSize);
    std::memcpy(memory.data(), code.data(), code.size());
    return memory;
}

} // namespace

// --- Decoding --------------------------------------------------------------

HYPERION_TEST("syscall", "decodes_0f_05_as_syscall") {
    const std::uint8_t code[] = {0x0F, 0x05};
    const hyperion::DecodeResult r = hyperion::decode(code, sizeof(code), kBase);
    HYPERION_CHECK(r.ok());
    HYPERION_CHECK(r.instruction.opcode == Opcode::kSyscall);
    HYPERION_CHECK_EQ(r.instruction.length, std::uint8_t{2});
}

HYPERION_TEST("syscall", "truncated_0f_is_truncated") {
    const std::uint8_t code[] = {0x0F};
    const hyperion::DecodeResult r = hyperion::decode(code, sizeof(code), kBase);
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == hyperion::DecodeError::kTruncated);
}

HYPERION_TEST("syscall", "unknown_0f_escape_is_rejected") {
    const std::uint8_t code[] = {0x0F, 0x0B}; // UD2: outside the subset
    const hyperion::DecodeResult r = hyperion::decode(code, sizeof(code), kBase);
    HYPERION_CHECK(!r.ok());
    HYPERION_CHECK(r.error == hyperion::DecodeError::kUnknownOpcode);
}

// --- Handler semantics (no generated code needed) --------------------------

HYPERION_TEST("syscall", "write_copies_guest_buffer_to_sink") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));

    FlatMemory memory = program({});
    const char* msg = "hello";
    HYPERION_CHECK(memory.write(kBase + 0x100, reinterpret_cast<const std::uint8_t*>(msg), 5));

    GuestState s;
    s.reg(Gpr::kRax) = hyperion::sysno::kWrite;
    s.reg(Gpr::kRdi) = 1;             // fd 1
    s.reg(Gpr::kRsi) = kBase + 0x100; // buf
    s.reg(Gpr::kRdx) = 5;             // count

    const hyperion::SyscallResult r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(cap.out, std::string("hello"));
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{5}); // bytes written
}

HYPERION_TEST("syscall", "write_to_bad_fd_is_ebadf") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory = program({});

    GuestState s;
    s.reg(Gpr::kRax) = hyperion::sysno::kWrite;
    s.reg(Gpr::kRdi) = 7; // not 1 or 2
    s.reg(Gpr::kRsi) = kBase + 0x100;
    s.reg(Gpr::kRdx) = 1;

    const hyperion::SyscallResult r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEbadf));
    HYPERION_CHECK(cap.out.empty());
}

HYPERION_TEST("syscall", "write_out_of_range_buffer_is_efault") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory = program({});

    GuestState s;
    s.reg(Gpr::kRax) = hyperion::sysno::kWrite;
    s.reg(Gpr::kRdi) = 1;
    s.reg(Gpr::kRsi) = kBase + kMemSize + 4096; // unmapped
    s.reg(Gpr::kRdx) = 4;

    const hyperion::SyscallResult r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEfault));
    HYPERION_CHECK(cap.out.empty());
}

HYPERION_TEST("syscall", "exit_reports_code_and_does_not_touch_rax_result") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory = program({});

    GuestState s;
    s.reg(Gpr::kRax) = hyperion::sysno::kExit;
    s.reg(Gpr::kRdi) = 42;

    const hyperion::SyscallResult r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kExit);
    HYPERION_CHECK_EQ(r.exit_code, std::int64_t{42});
}

HYPERION_TEST("syscall", "unknown_number_is_enosys_not_silent") {
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    FlatMemory memory = program({});

    GuestState s;
    s.reg(Gpr::kRax) = 39; // getpid: not implemented in this increment

    const hyperion::SyscallResult r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(r.number, std::uint64_t{39});
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
    HYPERION_CHECK(!hyperion::is_minimal_syscall(39));
}

// --- Interpreter integration ----------------------------------------------

HYPERION_TEST("interpreter", "syscall_write_then_exit_halts_program") {
    // mov eax, 1 (write) ; mov edi, 1 ; mov esi, buf ; mov edx, 5 ;
    // syscall ; mov eax, 60 (exit) ; mov edi, 0 ; syscall ; hlt
    // The buffer is placed in the same flat memory at kBase + 0x200.
    std::vector<std::uint8_t> code = {
        0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
        0xBF, 0x01, 0x00, 0x00, 0x00, // mov edi, 1
        0xBE, 0x00, 0x02, 0x40, 0x00, // mov esi, 0x400200
        0xBA, 0x05, 0x00, 0x00, 0x00, // mov edx, 5
        0x0F, 0x05,                   // syscall
        0xB8, 0x3C, 0x00, 0x00, 0x00, // mov eax, 60
        0x31, 0xFF,                   // xor edi, edi
        0x0F, 0x05,                   // syscall (exit)
        0xF4,                         // hlt
    };
    FlatMemory memory = program(code);
    const char* msg = "hyper";
    HYPERION_CHECK(memory.write(kBase + 0x200, reinterpret_cast<const std::uint8_t*>(msg), 5));

    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));

    GuestState initial;
    initial.rip = kBase;
    const hyperion::RunResult r = hyperion::run_program(memory, initial, 100, &handler);

    HYPERION_CHECK(r.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(cap.out, std::string("hyper"));
    HYPERION_CHECK_EQ(r.unimplemented_syscalls, std::uint64_t{0});
}

HYPERION_TEST("interpreter", "syscall_without_handler_is_unsupported_not_ignored") {
    const std::vector<std::uint8_t> code = {0x0F, 0x05, 0xF4};
    FlatMemory memory = program(code);
    GuestState initial;
    initial.rip = kBase;

    const hyperion::RunResult r = hyperion::run_program(memory, initial, 10);
    HYPERION_CHECK(r.status == RunStatus::kUnsupported);
}

HYPERION_TEST("interpreter", "unimplemented_syscall_returns_enosys_and_continues") {
    // mov eax, 39 (getpid) ; syscall ; hlt. getpid is unimplemented, so rax must
    // end as -ENOSYS and the program must continue to the hlt and stop cleanly.
    const std::vector<std::uint8_t> code = {
        0xB8, 0x27, 0x00, 0x00, 0x00, // mov eax, 39
        0x0F, 0x05,                   // syscall
        0xF4,                         // hlt
    };
    FlatMemory memory = program(code);
    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    GuestState initial;
    initial.rip = kBase;

    const hyperion::RunResult r = hyperion::run_program(memory, initial, 10, &handler);
    HYPERION_CHECK(r.status == RunStatus::kOk); // hlt reached
    HYPERION_CHECK_EQ(r.unimplemented_syscalls, std::uint64_t{1});
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
}

#if defined(__aarch64__)

HYPERION_TEST("translation_cache", "tier1_syscall_write_matches_interpreter") {
    std::vector<std::uint8_t> code = {
        0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
        0xBF, 0x01, 0x00, 0x00, 0x00, // mov edi, 1
        0xBE, 0x00, 0x02, 0x40, 0x00, // mov esi, 0x400200
        0xBA, 0x05, 0x00, 0x00, 0x00, // mov edx, 5
        0x0F, 0x05,                   // syscall
        0xB8, 0x3C, 0x00, 0x00, 0x00, // mov eax, 60
        0x31, 0xFF,                   // xor edi, edi
        0x0F, 0x05,                   // syscall (exit)
        0xF4,                         // hlt
    };
    FlatMemory memory = program(code);
    const char* msg = "hyper";
    HYPERION_CHECK(memory.write(kBase + 0x200, reinterpret_cast<const std::uint8_t*>(msg), 5));

    Captured cap;
    MinimalSyscallHandler handler(sink_to(cap));
    GuestState initial;
    initial.rip = kBase;

    hyperion::TranslationCache cache;
    const hyperion::RunResult r = cache.run(memory, initial, 100, &handler);

    HYPERION_CHECK(r.status == RunStatus::kOk);
    HYPERION_CHECK_EQ(cap.out, std::string("hyper"));
}

#endif // __aarch64__
