// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest Linux system-call layer (Phase 6, increment 1).
//
// The x86-64 `syscall` instruction (0F 05) transfers control to the kernel with
// the number in rax and arguments in rdi, rsi, rdx, r10, r8, r9. Hyperion runs
// guest code as an ordinary unprivileged host process, so there is no real
// kernel transition: the instruction is routed to a SyscallHandler that
// emulates the call in userspace and writes the result back into rax.
//
// Scope of this increment is deliberately tiny and stated rather than implied:
//   write(2), exit(2) and exit_group(2). Every other syscall number returns
//   -ENOSYS explicitly. Nothing silently succeeds. The full inventory and the
//   implementation order live in docs/syscalls/.
//
// Error convention: a failed syscall returns a *negative* errno in rax (e.g.
// -EFAULT), exactly as the Linux kernel does; it does not return -1 and set a
// host errno. This is what guest programs test for.
#ifndef HYPERION_SYSCALL_HPP
#define HYPERION_SYSCALL_HPP

#include <cstddef>
#include <cstdint>
#include <functional>

#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp" // Memory

namespace hyperion {

// x86-64 syscall numbers used by this increment. The authoritative list is
// docs/syscalls/kernel-table-x86_64-6.12.tsv; these are repeated here so the
// handler does not depend on the documentation.
namespace sysno {
inline constexpr std::uint64_t kWrite = 1;
inline constexpr std::uint64_t kExit = 60;
inline constexpr std::uint64_t kExitGroup = 231;
} // namespace sysno

// Negative errno values as the guest observes them (returned in rax).
inline constexpr std::int64_t kEbadf = 9;
inline constexpr std::int64_t kEfault = 14;
inline constexpr std::int64_t kEinval = 22;
inline constexpr std::int64_t kEnosys = 38;

// What the handler did with a syscall.
enum class SyscallStatus {
    kHandled,       // emulated: rax holds the result (or a negative errno)
    kUnimplemented, // not implemented: rax = -ENOSYS, execution continues
    kExit,          // the guest asked to terminate
};

const char* to_string(SyscallStatus status);

struct SyscallResult {
    SyscallStatus status = SyscallStatus::kUnimplemented;
    std::uint64_t number = 0;   // the syscall number requested
    std::int64_t exit_code = 0; // valid when status == kExit
};

// The guest system-call interface. An implementation receives the guest state
// with rax = number and rdi/rsi/rdx/r10/r8/r9 = arguments, and must write the
// result into rax.
class SyscallHandler {
public:
    virtual ~SyscallHandler() = default;
    virtual SyscallResult handle(GuestState& state, Memory& memory) = 0;
};

// A handler for the minimum needed to run a self-contained guest program that
// prints and terminates. `write` goes to a host sink (stdout for fd 1, stderr
// for fd 2); `exit`/`exit_group` stop the program; every other number returns
// -ENOSYS.
//
// The output sink is injectable so tests can capture guest output without
// touching the test runner's own stdout. The signature mirrors write(2): it
// returns the number of bytes written, or -1 on error.
class MinimalSyscallHandler final : public SyscallHandler {
public:
    using OutputSink = std::function<long(int fd, const std::uint8_t* data, std::size_t count)>;

    // With no sink, output is written to the process's stdout/stderr.
    explicit MinimalSyscallHandler(OutputSink sink = {});

    SyscallResult handle(GuestState& state, Memory& memory) override;

private:
    OutputSink sink_;
};

// True if MinimalSyscallHandler implements this syscall number (as opposed to
// returning -ENOSYS). Used by tests and by the compatibility matrix.
bool is_minimal_syscall(std::uint64_t number);

} // namespace hyperion

#endif // HYPERION_SYSCALL_HPP
