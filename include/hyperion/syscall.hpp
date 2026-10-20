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
//   write(2), read(2), close(2), lseek(2), exit(2), exit_group(2), and the
//   memory syscalls mmap(2), mprotect(2), munmap(2), brk(2). Every other syscall
//   number returns -ENOSYS explicitly. Nothing silently succeeds. In particular
//   open/openat are NOT implemented: the guest can read inherited descriptors
//   (stdin, or a descriptor the embedder adopted) but cannot open host files
//   yet. The full inventory and the implementation order live in docs/syscalls/.
//
// Error convention: a failed syscall returns a *negative* errno in rax (e.g.
// -EFAULT), exactly as the Linux kernel does; it does not return -1 and set a
// host errno. This is what guest programs test for.
#ifndef HYPERION_SYSCALL_HPP
#define HYPERION_SYSCALL_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "hyperion/address_space.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/interpreter.hpp" // Memory

namespace hyperion {

// x86-64 syscall numbers used by this increment. The authoritative list is
// docs/syscalls/kernel-table-x86_64-6.12.tsv; these are repeated here so the
// handler does not depend on the documentation.
namespace sysno {
inline constexpr std::uint64_t kRead = 0;
inline constexpr std::uint64_t kWrite = 1;
inline constexpr std::uint64_t kClose = 3;
inline constexpr std::uint64_t kLseek = 8;
inline constexpr std::uint64_t kMmap = 9;
inline constexpr std::uint64_t kMprotect = 10;
inline constexpr std::uint64_t kMunmap = 11;
inline constexpr std::uint64_t kBrk = 12;
inline constexpr std::uint64_t kExit = 60;
inline constexpr std::uint64_t kExitGroup = 231;
} // namespace sysno

// Guest PROT_* and MAP_* flags (the subset this increment honours). The values
// match the x86-64 Linux ABI, so a guest passing them gets the kernel's
// interpretation, not a reinterpretation.
namespace mmap_flags {
inline constexpr std::uint64_t kProtNone = 0x0;
inline constexpr std::uint64_t kProtRead = 0x1;
inline constexpr std::uint64_t kProtWrite = 0x2;
inline constexpr std::uint64_t kProtExec = 0x4;
inline constexpr std::uint64_t kMapShared = 0x01;
inline constexpr std::uint64_t kMapPrivate = 0x02;
inline constexpr std::uint64_t kMapAnonymous = 0x20;
inline constexpr std::uint64_t kMapFixed = 0x10;
} // namespace mmap_flags

// lseek(2) whence values.
namespace seek {
inline constexpr std::uint64_t kSet = 0;
inline constexpr std::uint64_t kCur = 1;
inline constexpr std::uint64_t kEnd = 2;
} // namespace seek

// Negative errno values as the guest observes them (returned in rax).
inline constexpr std::int64_t kEperm = 1;
inline constexpr std::int64_t kEnoent = 2;
inline constexpr std::int64_t kEio = 5;
inline constexpr std::int64_t kEbadf = 9;
inline constexpr std::int64_t kEnomem = 12;
inline constexpr std::int64_t kEacces = 13;
inline constexpr std::int64_t kEfault = 14;
inline constexpr std::int64_t kEinval = 22;
inline constexpr std::int64_t kEspipe = 29;
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
// prints, reads, allocates memory and terminates. `write` goes to a host sink
// (stdout for fd 1, stderr for fd 2); `read`/`close`/`lseek` operate on a small
// guest descriptor table the embedder can seed with adopted host descriptors
// (fd 0 is stdin by default); `mmap`/`mprotect`/`munmap`/`brk` operate on the
// supplied GuestAddressSpace; `exit`/`exit_group` stop the program; every other
// number returns -ENOSYS.
//
// The output sink is injectable so tests can capture guest output without
// touching the test runner's own stdout. The signature mirrors write(2): it
// returns the number of bytes written, or -1 on error.
//
// The address space is supplied by the caller (not owned), so the same
// GuestAddressSpace can be shared with the interpreter or translation cache and
// the handler has no hidden global state. When it is null, the memory syscalls
// return -ENOSYS rather than pretending to succeed.
//
// The descriptor table is owned by the handler (a guest file descriptor is
// handler-local state, like a real process's table). It is not a host file
// descriptor table: the handler maps guest fd -> host FILE* and enforces the
// mapping. `read`/`write` refuse a guest fd that is not in the table with
// -EBADF, so a guest cannot name an arbitrary host descriptor.
class MinimalSyscallHandler final : public SyscallHandler {
public:
    using OutputSink = std::function<long(int fd, const std::uint8_t* data, std::size_t count)>;

    // With no sink, output is written to the process's stdout/stderr. With no
    // address space, mmap/mprotect/munmap/brk are unimplemented. The descriptor
    // table starts with fd 0/1/2 mapped to the process's stdin/stdout/stderr;
    // see adopt() to add an already-open host stream under a guest fd.
    explicit MinimalSyscallHandler(OutputSink sink = {},
                                   GuestAddressSpace* address_space = nullptr);

    SyscallResult handle(GuestState& state, Memory& memory) override;

    // Map an already-open host stream to a guest descriptor so a guest can read
    // from it. This is how an embedder grants file access without implementing
    // open(2): the host side chooses the file, and the guest only ever sees an
    // integer. Ownership stays with the caller (the FILE* is not fclose'd by the
    // handler), matching the "borrowed, not owned" boundary. Fails (false) if
    // the fd is already taken or out of range.
    bool adopt(int guest_fd, std::FILE* stream);

private:
    OutputSink sink_;
    GuestAddressSpace* address_space_;
    // Guest fd -> host stream, or nullptr for a hole. Indexed by guest fd, so
    // the guest can never index out of range (the lookup is bounds-checked).
    std::vector<std::FILE*> files_;
};

// True if MinimalSyscallHandler implements this syscall number (as opposed to
// returning -ENOSYS). Used by tests and by the compatibility matrix.
bool is_minimal_syscall(std::uint64_t number);

} // namespace hyperion

#endif // HYPERION_SYSCALL_HPP
