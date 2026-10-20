// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Minimal guest syscall handler (Phase 6, increment 1). See syscall.hpp for the
// scope and the error convention.
#include "hyperion/syscall.hpp"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace hyperion {

const char* to_string(SyscallStatus status) {
    switch (status) {
    case SyscallStatus::kHandled:
        return "handled";
    case SyscallStatus::kUnimplemented:
        return "unimplemented";
    case SyscallStatus::kExit:
        return "exit";
    }
    return "?";
}

namespace {

// The x86-64 syscall argument registers, in order. r10 (not rcx) is the fourth
// argument: the `syscall` instruction itself clobbers rcx with the return
// address, so the ABI moves the fourth argument out of the way.
constexpr Gpr kArgRegs[6] = {Gpr::kRdi, Gpr::kRsi, Gpr::kRdx, Gpr::kR10, Gpr::kR8, Gpr::kR9};

std::uint64_t arg(const GuestState& s, int index) {
    return s.gpr[static_cast<std::size_t>(kArgRegs[index])];
}

// write(fd, buf, count): copy `count` guest bytes out and hand them to the
// sink. A guest pointer that the memory model rejects yields -EFAULT, not a
// host crash.
std::int64_t do_write(GuestState& s, Memory& memory,
                      const MinimalSyscallHandler::OutputSink& sink) {
    const std::int64_t fd = static_cast<std::int64_t>(arg(s, 0));
    const std::uint64_t buf = arg(s, 1);
    const std::uint64_t count = arg(s, 2);

    if (fd != 1 && fd != 2) {
        return -kEbadf;
    }
    if (count == 0) {
        return 0;
    }

    // Copy the guest buffer in bounded chunks. `count` is guest-controlled, so
    // it is not trusted to be small.
    constexpr std::size_t kChunk = 4096;
    std::array<std::uint8_t, kChunk> chunk{};
    std::uint64_t done = 0;
    while (done < count) {
        const std::size_t n =
            static_cast<std::size_t>((count - done) < kChunk ? (count - done) : kChunk);
        if (!memory.read(buf + done, chunk.data(), n)) {
            return -kEfault;
        }
        if (sink) {
            const long wrote = sink(static_cast<int>(fd), chunk.data(), n);
            if (wrote < 0) {
                return -kEbadf;
            }
        }
        done += n;
    }
    return static_cast<std::int64_t>(done);
}

long default_sink(int fd, const std::uint8_t* data, std::size_t count) {
    std::FILE* out = (fd == 2) ? stderr : stdout;
    const std::size_t wrote = std::fwrite(data, 1, count, out);
    std::fflush(out);
    return static_cast<long>(wrote);
}

} // namespace

MinimalSyscallHandler::MinimalSyscallHandler(OutputSink sink)
    : sink_(sink ? std::move(sink) : OutputSink(default_sink)) {}

SyscallResult MinimalSyscallHandler::handle(GuestState& state, Memory& memory) {
    SyscallResult result;
    result.number = state.reg(Gpr::kRax);

    switch (result.number) {
    case sysno::kWrite: {
        const std::int64_t r = do_write(state, memory, sink_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kExit:
    case sysno::kExitGroup:
        result.status = SyscallStatus::kExit;
        result.exit_code = static_cast<std::int64_t>(arg(state, 0)) & 0xFF;
        return result;
    default:
        // Explicit, never silent: the guest sees -ENOSYS and can decide what to
        // do. A stub is never reported as support.
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(-kEnosys);
        result.status = SyscallStatus::kUnimplemented;
        return result;
    }
}

bool is_minimal_syscall(std::uint64_t number) {
    return number == sysno::kWrite || number == sysno::kExit || number == sysno::kExitGroup;
}

} // namespace hyperion
