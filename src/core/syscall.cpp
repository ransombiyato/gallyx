// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Minimal guest syscall handler (Phase 6). See syscall.hpp for the scope and
// the error convention.
#include "hyperion/syscall.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

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

// Look up a guest descriptor in the table. Returns nullptr for a descriptor that
// is negative, out of range, or a hole; callers map that to -EBADF. Keeping the
// conversion in one place means no signed index ever reaches vector::operator[].
std::FILE* lookup_stream(const std::vector<std::FILE*>& files, std::int64_t fd) {
    if (fd < 0) {
        return nullptr;
    }
    const auto idx = static_cast<std::size_t>(fd);
    if (idx >= files.size()) {
        return nullptr;
    }
    return files[idx];
}

// read(fd, buf, count): read `count` guest bytes from the adopted host stream
// into guest memory. A guest pointer the memory model rejects yields -EFAULT; a
// descriptor that is not in the table yields -EBADF. The host read is bounded
// by the chunk size so a huge `count` cannot ask for one unbounded allocation.
std::int64_t do_read(GuestState& s, Memory& memory, const std::vector<std::FILE*>& files) {
    const std::int64_t fd = static_cast<std::int64_t>(arg(s, 0));
    const std::uint64_t buf = arg(s, 1);
    const std::uint64_t count = arg(s, 2);

    std::FILE* f = lookup_stream(files, fd);
    if (f == nullptr) {
        return -kEbadf;
    }
    if (count == 0) {
        return 0;
    }

    // Read in bounded chunks and copy each chunk into the guest address space.
    // The Memory interface cannot validate a range without touching it, so a
    // fault on the *destination* is discovered when the first chunk is stored;
    // at that point any bytes already read from the stream are consumed (a real
    // kernel would fault before reading, but this is a documented limitation of
    // the chunked approach, not a silent wrong answer: the guest gets -EFAULT).
    constexpr std::size_t kChunk = 4096;
    std::array<std::uint8_t, kChunk> chunk{};
    std::uint64_t done = 0;
    while (done < count) {
        const std::size_t n =
            static_cast<std::size_t>((count - done) < kChunk ? (count - done) : kChunk);
        const std::size_t got = std::fread(chunk.data(), 1, n, f);
        if (got == 0) {
            break; // EOF or error: a short read is the result, as with read(2)
        }
        if (!memory.write(buf + done, chunk.data(), got)) {
            return -kEfault;
        }
        done += got;
        if (got < n) {
            break; // short read: the stream is exhausted
        }
    }
    return static_cast<std::int64_t>(done);
}

// close(fd): drop the guest descriptor. The handler does not own the underlying
// host stream, so it is not fclose'd (the embedder does that); it is only
// removed from the guest's view.
std::int64_t do_close(GuestState& s, std::vector<std::FILE*>& files) {
    const std::int64_t fd = static_cast<std::int64_t>(arg(s, 0));
    if (lookup_stream(files, fd) == nullptr) {
        return -kEbadf;
    }
    files[static_cast<std::size_t>(fd)] = nullptr;
    return 0;
}

// lseek(fd, offset, whence): reposition an adopted stream. Only seekable
// streams are supported; a pipe/terminal (stdin from a tty) is ESPIPE, exactly
// as the kernel reports. A negative resulting offset is EINVAL.
std::int64_t do_lseek(GuestState& s, const std::vector<std::FILE*>& files) {
    const std::int64_t fd = static_cast<std::int64_t>(arg(s, 0));
    const std::int64_t offset = static_cast<std::int64_t>(arg(s, 1));
    const std::uint64_t whence = arg(s, 2);

    std::FILE* f = lookup_stream(files, fd);
    if (f == nullptr) {
        return -kEbadf;
    }
    int host_whence = 0;
    switch (whence) {
    case seek::kSet:
        host_whence = SEEK_SET;
        break;
    case seek::kCur:
        host_whence = SEEK_CUR;
        break;
    case seek::kEnd:
        host_whence = SEEK_END;
        break;
    default:
        return -kEinval;
    }
    if (std::fseek(f, static_cast<long>(offset), host_whence) != 0) {
        return -kEspipe; // not seekable (pipe/tty), or an invalid position
    }
    const long pos = std::ftell(f);
    if (pos < 0) {
        return -kEinval;
    }
    return static_cast<std::int64_t>(pos);
}

// Translate the guest PROT_* bits into the address space's MemPerm. Unknown bits
// are ignored, as the kernel does; PROT_NONE (no bits) maps to kNone.
MemPerm prot_to_perm(std::uint64_t prot) {
    MemPerm perm = MemPerm::kNone;
    if (prot & mmap_flags::kProtRead) {
        perm = perm | MemPerm::kRead;
    }
    if (prot & mmap_flags::kProtWrite) {
        perm = perm | MemPerm::kWrite;
    }
    if (prot & mmap_flags::kProtExec) {
        perm = perm | MemPerm::kExec;
    }
    return perm;
}

// A memory-syscall outcome: the value to place in rax, plus whether the call
// was actually implemented. Keeping these separate is what stops an
// unimplemented request (e.g. a file-backed mmap) from being reported as a
// handled call with a negative return: the dispatcher must count it as
// unimplemented so it is never invisible.
struct MemResult {
    std::int64_t value = 0;
    bool implemented = true;
};

// mmap(addr, length, prot, flags, fd, offset). This increment supports
// anonymous private mappings only; a file-backed mapping (no MAP_ANONYMOUS) is
// reported unimplemented rather than mapping something wrong. A zero-length
// request is -EINVAL, as the kernel does. For MAP_ANONYMOUS the kernel ignores
// fd and offset, so they are not inspected here.
MemResult do_mmap(GuestState& s, GuestAddressSpace& as) {
    const std::uint64_t addr = arg(s, 0);
    const std::uint64_t length = arg(s, 1);
    const std::uint64_t prot = arg(s, 2);
    const std::uint64_t flags = arg(s, 3);

    if (length == 0) {
        return {-kEinval, true}; // the kernel rejects a zero-length mapping
    }
    if ((flags & mmap_flags::kMapAnonymous) == 0) {
        return {-kEnosys, false}; // file-backed mappings are not implemented yet
    }
    const bool fixed = (flags & mmap_flags::kMapFixed) != 0;
    const auto got =
        as.map_anonymous(static_cast<std::size_t>(length), prot_to_perm(prot), fixed ? addr : 0);
    if (!got) {
        return {-kEnomem, true};
    }
    return {static_cast<std::int64_t>(*got), true};
}

MemResult do_mprotect(GuestState& s, GuestAddressSpace& as) {
    const std::uint64_t addr = arg(s, 0);
    const std::uint64_t len = arg(s, 1);
    const std::uint64_t prot = arg(s, 2);
    if (len == 0 || (addr & GuestAddressSpace::kPageMask) != 0) {
        return {-kEinval, true};
    }
    if (!as.protect(addr, static_cast<std::size_t>(len), prot_to_perm(prot))) {
        return {-kEnomem, true}; // an unmapped or misaligned range
    }
    return {0, true};
}

MemResult do_munmap(GuestState& s, GuestAddressSpace& as) {
    const std::uint64_t addr = arg(s, 0);
    const std::uint64_t len = arg(s, 1);
    if (len == 0 || (addr & GuestAddressSpace::kPageMask) != 0) {
        return {-kEinval, true};
    }
    if (!as.unmap(addr, static_cast<std::size_t>(len))) {
        return {-kEinval, true}; // partial unmapping is not supported yet
    }
    return {0, true};
}

MemResult do_brk(GuestState& s, GuestAddressSpace& as) {
    const std::uint64_t requested = arg(s, 0);
    return {static_cast<std::int64_t>(as.set_brk(requested)), true};
}

} // namespace

MinimalSyscallHandler::MinimalSyscallHandler(OutputSink sink, GuestAddressSpace* address_space)
    : sink_(sink ? std::move(sink) : OutputSink(default_sink)), address_space_(address_space) {
    // The standard descriptors exist from the start, as they do for a real
    // process: 0 = stdin, 1 = stdout, 2 = stderr. `write` still routes 1/2 to the
    // sink; `read`/`lseek` use the stream table, so a guest that reads fd 0 gets
    // the process's stdin (which a test can replace with adopt()).
    files_ = {stdin, stdout, stderr};
}

bool MinimalSyscallHandler::adopt(int guest_fd, std::FILE* stream) {
    if (guest_fd < 0 || stream == nullptr) {
        return false;
    }
    const auto idx = static_cast<std::size_t>(guest_fd);
    if (idx < files_.size() && files_[idx] != nullptr) {
        return false; // already occupied
    }
    if (idx >= files_.size()) {
        files_.resize(idx + 1, nullptr);
    }
    files_[idx] = stream;
    return true;
}

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
    case sysno::kRead: {
        const std::int64_t r = do_read(state, memory, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kClose: {
        const std::int64_t r = do_close(state, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kLseek: {
        const std::int64_t r = do_lseek(state, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kExit:
    case sysno::kExitGroup:
        result.status = SyscallStatus::kExit;
        result.exit_code = static_cast<std::int64_t>(arg(state, 0)) & 0xFF;
        return result;
    case sysno::kMmap:
    case sysno::kMprotect:
    case sysno::kMunmap:
    case sysno::kBrk: {
        // The address-space syscalls require an address space. Without one the
        // handler reports the call unimplemented rather than pretending success,
        // so a missing address space is never invisible.
        if (address_space_ == nullptr) {
            state.reg(Gpr::kRax) = static_cast<std::uint64_t>(-kEnosys);
            result.status = SyscallStatus::kUnimplemented;
            return result;
        }
        std::int64_t r = 0;
        bool implemented = true;
        switch (result.number) {
        case sysno::kMmap: {
            const MemResult mr = do_mmap(state, *address_space_);
            r = mr.value;
            implemented = mr.implemented;
            break;
        }
        case sysno::kMprotect:
            r = do_mprotect(state, *address_space_).value;
            break;
        case sysno::kMunmap:
            r = do_munmap(state, *address_space_).value;
            break;
        default:
            r = do_brk(state, *address_space_).value;
            break;
        }
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = implemented ? SyscallStatus::kHandled : SyscallStatus::kUnimplemented;
        return result;
    }
    default:
        // Explicit, never silent: the guest sees -ENOSYS and can decide what to
        // do. A stub is never reported as support.
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(-kEnosys);
        result.status = SyscallStatus::kUnimplemented;
        return result;
    }
}

bool is_minimal_syscall(std::uint64_t number) {
    return number == sysno::kRead || number == sysno::kWrite || number == sysno::kClose ||
           number == sysno::kLseek || number == sysno::kExit || number == sysno::kExitGroup ||
           number == sysno::kMmap || number == sysno::kMprotect || number == sysno::kMunmap ||
           number == sysno::kBrk;
}

} // namespace hyperion
