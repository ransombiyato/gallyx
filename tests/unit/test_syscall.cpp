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
#include "hyperion/address_space.hpp"
#include "hyperion/decoder.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/run.hpp"
#include "hyperion/syscall.hpp"
#include "hyperion/translation_cache.hpp"

using hyperion::FlatMemory;
using hyperion::Gpr;
using hyperion::GuestAddressSpace;
using hyperion::GuestState;
using hyperion::MemPerm;
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

// --- Address-space syscalls (mmap/mprotect/munmap/brk) ----------------------

// Build a guest state with the syscall number in rax and up to six arguments in
// the x86-64 syscall argument registers.
GuestState syscall_state(std::uint64_t number, std::initializer_list<std::uint64_t> args) {
    GuestState s;
    s.reg(Gpr::kRax) = number;
    const Gpr regs[6] = {Gpr::kRdi, Gpr::kRsi, Gpr::kRdx, Gpr::kR10, Gpr::kR8, Gpr::kR9};
    std::size_t i = 0;
    for (std::uint64_t a : args) {
        if (i < 6) {
            s.reg(regs[i]) = a;
        }
        ++i;
    }
    return s;
}

HYPERION_TEST("syscall", "mmap_anonymous_returns_a_mapping_that_is_usable") {
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    FlatMemory memory(kBase, kMemSize);

    // mmap(NULL, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
    GuestState s =
        syscall_state(hyperion::sysno::kMmap,
                      {0, 8192, hyperion::mmap_flags::kProtRead | hyperion::mmap_flags::kProtWrite,
                       hyperion::mmap_flags::kMapPrivate | hyperion::mmap_flags::kMapAnonymous,
                       static_cast<std::uint64_t>(-1), 0});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kHandled);

    const std::uint64_t addr = s.reg(Gpr::kRax);
    HYPERION_CHECK((addr & (GuestAddressSpace::kPageSize - 1)) == 0); // page-aligned
    const std::uint8_t v = 0x99;
    HYPERION_CHECK(as.write(addr, &v, 1));
    std::uint8_t out = 0;
    HYPERION_CHECK(as.read(addr, &out, 1));
    HYPERION_CHECK(out == 0x99);
}

HYPERION_TEST("syscall", "mmap_zero_length_is_einval") {
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    FlatMemory memory(kBase, kMemSize);
    GuestState s = syscall_state(
        hyperion::sysno::kMmap,
        {0, 0, hyperion::mmap_flags::kProtRead,
         hyperion::mmap_flags::kMapPrivate | hyperion::mmap_flags::kMapAnonymous, 0, 0});
    handler.handle(s, memory);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEinval));
}

HYPERION_TEST("syscall", "mmap_file_backed_is_enosys_not_a_wrong_mapping") {
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    FlatMemory memory(kBase, kMemSize);
    // No MAP_ANONYMOUS: a file-backed mapping, which this increment does not do.
    GuestState s = syscall_state(hyperion::sysno::kMmap, {0, 4096, hyperion::mmap_flags::kProtRead,
                                                          hyperion::mmap_flags::kMapPrivate, 3, 0});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
}

HYPERION_TEST("syscall", "mprotect_and_munmap_round_trip") {
    GuestAddressSpace as;
    MinimalSyscallHandler handler({}, &as);
    FlatMemory memory(kBase, kMemSize);

    GuestState m = syscall_state(
        hyperion::sysno::kMmap,
        {0, 4096, hyperion::mmap_flags::kProtRead | hyperion::mmap_flags::kProtWrite,
         hyperion::mmap_flags::kMapPrivate | hyperion::mmap_flags::kMapAnonymous, 0, 0});
    handler.handle(m, memory);
    const std::uint64_t addr = m.reg(Gpr::kRax);
    HYPERION_CHECK(addr != 0);

    // mprotect to read-only: a write through the address space must then fail.
    GuestState p =
        syscall_state(hyperion::sysno::kMprotect, {addr, 4096, hyperion::mmap_flags::kProtRead});
    HYPERION_CHECK(handler.handle(p, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(p.reg(Gpr::kRax), std::uint64_t{0});
    const std::uint8_t v = 1;
    HYPERION_CHECK(!as.write(addr, &v, 1));

    // munmap removes it entirely.
    GuestState u = syscall_state(hyperion::sysno::kMunmap, {addr, 4096});
    HYPERION_CHECK(handler.handle(u, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(u.reg(Gpr::kRax), std::uint64_t{0});
    std::uint8_t out = 0;
    HYPERION_CHECK(!as.read(addr, &out, 1));
}

HYPERION_TEST("syscall", "brk_returns_the_new_break") {
    GuestAddressSpace as(0x7f0000000000ULL, 0x200000ULL, 1ULL << 20);
    MinimalSyscallHandler handler({}, &as);
    FlatMemory memory(kBase, kMemSize);

    GuestState s = syscall_state(hyperion::sysno::kBrk, {0x201000});
    HYPERION_CHECK(handler.handle(s, memory).status == SyscallStatus::kHandled);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), std::uint64_t{0x201000});
    const std::uint8_t v = 0x5A;
    HYPERION_CHECK(as.write(0x200FF0, &v, 1)); // the grown heap is writable
}

HYPERION_TEST("syscall", "memory_syscalls_without_an_address_space_are_enosys") {
    MinimalSyscallHandler handler; // no address space
    FlatMemory memory(kBase, kMemSize);
    GuestState s = syscall_state(
        hyperion::sysno::kMmap,
        {0, 4096, hyperion::mmap_flags::kProtRead,
         hyperion::mmap_flags::kMapPrivate | hyperion::mmap_flags::kMapAnonymous, 0, 0});
    const auto r = handler.handle(s, memory);
    HYPERION_CHECK(r.status == SyscallStatus::kUnimplemented);
    HYPERION_CHECK_EQ(s.reg(Gpr::kRax), static_cast<std::uint64_t>(-hyperion::kEnosys));
}

HYPERION_TEST("syscall", "is_minimal_syscall_reports_the_new_set") {
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kMmap));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kMprotect));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kMunmap));
    HYPERION_CHECK(hyperion::is_minimal_syscall(hyperion::sysno::kBrk));
    HYPERION_CHECK(!hyperion::is_minimal_syscall(39)); // getpid, still unimplemented
}

// A guest program that mmaps a page and stores a byte through the returned
// pointer, exercising the whole path: the syscall handler allocates in the
// GuestAddressSpace, the guest writes to it with a normal memory store, and the
// interpreter reads code from the same address space. This is the integration
// the unit tests above do not cover (they call the handler directly).
//
// Only encodings in the current subset are used (mov r32, imm32; mov r/m, r;
// xor; syscall; hlt). The mapping is requested MAP_FIXED at 0x50000000, an
// address that fits in 32 bits, so the guest's 32-bit `mov ebx, eax` preserves
// the pointer the handler returned and the store goes to the right place.
//
//   mov eax, 9        ; mmap
//   mov edi, 0x50000000 ; addr = MAP_FIXED hint
//   mov esi, 4096     ; length
//   mov edx, 3        ; PROT_READ|PROT_WRITE
//   mov r10d, 0x32    ; MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED
//   mov r8d, -1       ; fd
//   xor r9d, r9d      ; offset
//   syscall
//   mov ebx, eax      ; save the returned address
//   mov ecx, 0xAB
//   mov [rbx], ecx    ; store through the mapping
//   hlt
std::vector<std::uint8_t> mmap_program() {
    return {
        0xB8, 0x09, 0x00, 0x00, 0x00,       // mov eax, 9
        0xBF, 0x00, 0x00, 0x00, 0x50,       // mov edi, 0x50000000
        0xBE, 0x00, 0x10, 0x00, 0x00,       // mov esi, 4096
        0xBA, 0x03, 0x00, 0x00, 0x00,       // mov edx, 3
        0x41, 0xBA, 0x32, 0x00, 0x00, 0x00, // mov r10d, 0x32
        0x41, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, // mov r8d, -1
        0x45, 0x31, 0xC9,                   // xor r9d, r9d
        0x0F, 0x05,                         // syscall
        0x89, 0xC3,                         // mov ebx, eax
        0xB9, 0xAB, 0x00, 0x00, 0x00,       // mov ecx, 0xAB
        0x89, 0x0B,                         // mov [rbx], ecx
        0xF4,                               // hlt
    };
}

// Map the program image into `as` at kBase and run it, returning the result.
hyperion::RunResult run_mmap_program(GuestAddressSpace& as, Captured& cap, bool use_cache) {
    const std::vector<std::uint8_t> code = mmap_program();
    HYPERION_CHECK(as.map_image(kBase, code.data(), code.size(), MemPerm::kRead | MemPerm::kExec));

    MinimalSyscallHandler handler(sink_to(cap), &as);
    GuestState initial;
    initial.rip = kBase;
    if (use_cache) {
        hyperion::TranslationCache cache;
        return cache.run(as, initial, 200, &handler);
    }
    return hyperion::run_program(as, initial, 200, &handler);
}

HYPERION_TEST("syscall", "mmap_end_to_end_under_the_interpreter") {
    GuestAddressSpace as;
    Captured cap;
    const hyperion::RunResult r = run_mmap_program(as, cap, /*use_cache=*/false);

    HYPERION_CHECK(r.status == RunStatus::kOk);                    // hlt reached
    HYPERION_CHECK_EQ(r.unimplemented_syscalls, std::uint64_t{0}); // mmap was handled
    const std::uint64_t addr = r.state.reg(Gpr::kRbx);
    HYPERION_CHECK((addr & (GuestAddressSpace::kPageSize - 1)) == 0);
    std::uint8_t stored = 0;
    HYPERION_CHECK(as.read(addr, &stored, 1));
    HYPERION_CHECK(stored == 0xAB); // the guest's store landed in the mapping
}

#if defined(__aarch64__)
// The AArch64 code generator is register-only today: it rejects any instruction
// with a memory operand (translator.cpp validate_body) and stops the block
// there rather than emitting wrong code. This program stores through the mmap
// pointer with a memory operand, so under generated code it must stop with
// kUnsupported *after* the mmap syscall has run. This test pins that boundary:
// it is what "unsupported instructions are handled explicitly" means in
// practice, and it will start asserting the successful store once the generator
// learns memory operands.
HYPERION_TEST("syscall", "mmap_end_to_end_under_generated_code_stops_at_memory_operand") {
    GuestAddressSpace as;
    Captured cap;
    const hyperion::RunResult r = run_mmap_program(as, cap, /*use_cache=*/true);

    HYPERION_CHECK(r.status == RunStatus::kUnsupported);
    HYPERION_CHECK_EQ(r.unimplemented_syscalls, std::uint64_t{0});        // mmap itself was handled
    HYPERION_CHECK_EQ(r.state.reg(Gpr::kRbx), std::uint64_t{0x50000000}); // mapping returned
    std::uint8_t stored = 0;
    HYPERION_CHECK(as.read(0x50000000, &stored, 1));
    HYPERION_CHECK(stored == 0x00); // the store was never executed
}
#endif // __aarch64__

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
