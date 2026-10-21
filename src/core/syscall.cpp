// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Minimal guest syscall handler (Phase 6). See syscall.hpp for the scope and
// the error convention.
#include "hyperion/syscall.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
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
const GuestDescriptor* lookup_fd(const std::vector<GuestDescriptor>& files, std::int64_t fd) {
    if (fd < 0) {
        return nullptr;
    }
    const auto idx = static_cast<std::size_t>(fd);
    if (idx >= files.size() || files[idx].stream == nullptr) {
        return nullptr;
    }
    return &files[idx];
}

std::FILE* lookup_stream(const std::vector<GuestDescriptor>& files, std::int64_t fd) {
    const GuestDescriptor* e = lookup_fd(files, fd);
    return e == nullptr ? nullptr : e->stream;
}

// Allocate the lowest free guest descriptor >= 3 for an opened stream, as the
// kernel does. Returns -EMFILE when the table is exhausted.
std::int64_t alloc_fd(std::vector<GuestDescriptor>& files, std::FILE* stream, bool owned) {
    for (std::size_t i = 3; i < files.size(); ++i) {
        if (files[i].stream == nullptr) {
            files[i] = GuestDescriptor{stream, owned};
            return static_cast<std::int64_t>(i);
        }
    }
    // Grow the table, but bound it so a runaway guest cannot exhaust memory.
    constexpr std::size_t kMaxGuestFds = 4096;
    if (files.size() >= kMaxGuestFds) {
        return -kEmfile;
    }
    files.push_back(GuestDescriptor{stream, owned});
    return static_cast<std::int64_t>(files.size() - 1);
}

// read(fd, buf, count): read `count` guest bytes from the adopted host stream
// into guest memory. A guest pointer the memory model rejects yields -EFAULT; a
// descriptor that is not in the table yields -EBADF. The host read is bounded
// by the chunk size so a huge `count` cannot ask for one unbounded allocation.
std::int64_t do_read(GuestState& s, Memory& memory, const std::vector<GuestDescriptor>& files) {
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

// close(fd): drop the guest descriptor. A stream the handler opened is closed;
// one the embedder adopted is only removed from the guest's view (its owner
// closes it).
std::int64_t do_close(GuestState& s, std::vector<GuestDescriptor>& files) {
    const std::int64_t fd = static_cast<std::int64_t>(arg(s, 0));
    const GuestDescriptor* e = lookup_fd(files, fd);
    if (e == nullptr) {
        return -kEbadf;
    }
    // Close a stream the handler opened; leave an adopted one for its owner.
    if (e->owned) {
        std::fclose(e->stream);
    }
    files[static_cast<std::size_t>(fd)] = GuestDescriptor{};
    return 0;
}

// lseek(fd, offset, whence): reposition an adopted stream. Only seekable
// streams are supported; a pipe/terminal (stdin from a tty) is ESPIPE, exactly
// as the kernel reports. A negative resulting offset is EINVAL.
std::int64_t do_lseek(GuestState& s, const std::vector<GuestDescriptor>& files) {
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

// Read a NUL-terminated guest string into `out`. The guest controls the length,
// so the copy is bounded by kMaxPath: a missing terminator within that bound is
// -ENAMETOOLONG, not an unbounded read. A pointer the memory model rejects is
// -EFAULT. Returns 0 on success, or a negative errno.
std::int64_t read_guest_cstr(Memory& memory, std::uint64_t address, std::string& out) {
    constexpr std::size_t kMaxPath = 4096; // Linux PATH_MAX
    out.clear();
    out.reserve(64);
    for (std::size_t i = 0; i < kMaxPath; ++i) {
        std::uint8_t ch = 0;
        if (!memory.read(address + i, &ch, 1)) {
            return -kEfault;
        }
        if (ch == 0) {
            return 0;
        }
        out.push_back(static_cast<char>(ch));
    }
    return -kEnametoolong;
}

// Map the guest O_* bits to the host's. Only the flags this increment honours
// are translated; an unhandled flag is reported as unimplemented rather than
// silently dropped, so a guest never gets weaker semantics than it asked for.
struct OpenFlags {
    int host = 0;
    bool implemented = true;
};

OpenFlags translate_open_flags(std::uint64_t flags) {
    OpenFlags r;
    switch (flags & open_flags::kOAccmode) {
    case open_flags::kORdonly:
        r.host |= O_RDONLY;
        break;
    case open_flags::kOWronly:
        r.host |= O_WRONLY;
        break;
    case open_flags::kORdwr:
        r.host |= O_RDWR;
        break;
    default:
        return {0, false}; // the reserved access mode (3) is invalid
    }
    if ((flags & open_flags::kOCreat) != 0) {
        r.host |= O_CREAT;
    }
    if ((flags & open_flags::kOTrunc) != 0) {
        r.host |= O_TRUNC;
    }
    if ((flags & open_flags::kOAppend) != 0) {
        r.host |= O_APPEND;
    }
    if ((flags & open_flags::kOExcl) != 0) {
        r.host |= O_EXCL;
    }
    // Anything outside the honoured set (O_DIRECTORY, O_NONBLOCK, O_CLOEXEC, …)
    // is not silently ignored: the call is reported unimplemented.
    constexpr std::uint64_t kHonoured = open_flags::kOAccmode | open_flags::kOCreat |
                                        open_flags::kOTrunc | open_flags::kOAppend |
                                        open_flags::kOExcl;
    if ((flags & ~kHonoured) != 0) {
        return {0, false};
    }
    return r;
}

// openat(dirfd, pathname, flags, mode). Only AT_FDCWD is supported: a real
// directory descriptor is a *at() feature this increment does not implement, so
// it returns -ENOSYS rather than resolving against the wrong base. The path is
// read from guest memory (bounded, -EFAULT on a bad pointer) and handed to the
// host kernel; this is not a sandbox (see SECURITY.md).
std::int64_t do_openat(GuestState& s, Memory& memory, std::vector<GuestDescriptor>& files) {
    const std::int64_t dirfd = static_cast<std::int64_t>(arg(s, 0));
    const std::uint64_t path_addr = arg(s, 1);
    const std::uint64_t flags = arg(s, 2);
    const std::uint64_t mode = arg(s, 3);

    if (static_cast<std::uint64_t>(dirfd) != kAtFdcwd) {
        return -kEnosys; // only AT_FDCWD is implemented
    }
    std::string path;
    const std::int64_t rc = read_guest_cstr(memory, path_addr, path);
    if (rc != 0) {
        return rc;
    }
    const OpenFlags of = translate_open_flags(flags);
    if (!of.implemented) {
        return -kEnosys;
    }

    // Use the low-level open() so no guest-supplied name is interpreted by the
    // C library's own path handling; O_CREAT/O_EXCL need the mode argument.
    const int host_fd = ::open(path.c_str(), of.host | O_CLOEXEC, static_cast<mode_t>(mode));
    if (host_fd < 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    std::FILE* f =
        ::fdopen(host_fd, (of.host & O_RDWR) != 0 ? "r+" : ((of.host & O_WRONLY) != 0 ? "w" : "r"));
    if (f == nullptr) {
        const int saved = errno;
        ::close(host_fd);
        return -static_cast<std::int64_t>(saved == 0 ? kEio : saved);
    }
    const std::int64_t gfd = alloc_fd(files, f, /*owned=*/true);
    if (gfd < 0) {
        std::fclose(f);
        return gfd;
    }
    return gfd;
}

// Write a std::uint64_t/32-bit field into a byte buffer in host order. The guest
// is x86-64 (little-endian) and every supported host is little-endian, so a
// direct store is correct; this keeps the field writes explicit and type-safe.
void put_u64(std::uint8_t* buf, std::size_t off, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        buf[off + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

void put_u32(std::uint8_t* buf, std::size_t off, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        buf[off + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

// fstat(fd, statbuf): report metadata for a guest descriptor. The guest is
// x86-64 and the host may be AArch64, whose struct stat has a different layout,
// so the guest struct is built field by field from the host's — never memcpy'd.
std::int64_t do_fstat(GuestState& s, Memory& memory, const std::vector<GuestDescriptor>& files) {
    const std::int64_t fd = static_cast<std::int64_t>(arg(s, 0));
    const std::uint64_t buf = arg(s, 1);

    std::FILE* f = lookup_stream(files, fd);
    if (f == nullptr) {
        return -kEbadf;
    }
    struct stat st{};
    if (::fstat(::fileno(f), &st) != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }

    std::array<std::uint8_t, guest_stat::kSize> out{};
    put_u64(out.data(), guest_stat::kOffStDev, static_cast<std::uint64_t>(st.st_dev));
    put_u64(out.data(), guest_stat::kOffStIno, static_cast<std::uint64_t>(st.st_ino));
    put_u64(out.data(), guest_stat::kOffStNlink, static_cast<std::uint64_t>(st.st_nlink));
    put_u32(out.data(), guest_stat::kOffStMode, static_cast<std::uint32_t>(st.st_mode));
    put_u32(out.data(), guest_stat::kOffStUid, static_cast<std::uint32_t>(st.st_uid));
    put_u32(out.data(), guest_stat::kOffStGid, static_cast<std::uint32_t>(st.st_gid));
    put_u64(out.data(), guest_stat::kOffStRdev, static_cast<std::uint64_t>(st.st_rdev));
    put_u64(out.data(), guest_stat::kOffStSize, static_cast<std::uint64_t>(st.st_size));
    put_u64(out.data(), guest_stat::kOffStBlksize, static_cast<std::uint64_t>(st.st_blksize));
    put_u64(out.data(), guest_stat::kOffStBlocks, static_cast<std::uint64_t>(st.st_blocks));
    // Guest timespec fields (tv_sec, tv_nsec), each 16 bytes. The host field
    // names differ (st_atim on Linux), so copy through a local.
    put_u64(out.data(), guest_stat::kOffStAtime,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(st.st_atim.tv_sec)));
    put_u64(out.data(), guest_stat::kOffStAtime + 8,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(st.st_atim.tv_nsec)));
    put_u64(out.data(), guest_stat::kOffStMtime,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(st.st_mtim.tv_sec)));
    put_u64(out.data(), guest_stat::kOffStMtime + 8,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(st.st_mtim.tv_nsec)));
    put_u64(out.data(), guest_stat::kOffStCtime,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(st.st_ctim.tv_sec)));
    put_u64(out.data(), guest_stat::kOffStCtime + 8,
            static_cast<std::uint64_t>(static_cast<std::int64_t>(st.st_ctim.tv_nsec)));

    if (!memory.write(buf, out.data(), out.size())) {
        return -kEfault;
    }
    return 0;
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

// getrandom(buf, buflen, flags): fill a guest buffer with random bytes from the
// host. Only the two defined flag bits (GRND_NONBLOCK, GRND_RANDOM) are
// accepted; any other bit is -EINVAL, as the kernel does. Hyperion cannot block,
// so the flags are validated and otherwise ignored: the host call blocks, which
// is what a guest that did not pass GRND_NONBLOCK asked for.
std::int64_t do_getrandom(GuestState& s, Memory& memory) {
    const std::uint64_t buf = arg(s, 0);
    const std::uint64_t count = arg(s, 1);
    const std::uint64_t flags = arg(s, 2);

    constexpr std::uint64_t kHonoured = 0x1 | 0x2; // GRND_NONBLOCK | GRND_RANDOM
    if ((flags & ~kHonoured) != 0) {
        return -kEinval;
    }
    if (count == 0) {
        return 0;
    }

    // `count` is guest-controlled, so copy in bounded chunks rather than
    // allocating it in one go.
    constexpr std::size_t kChunk = 4096;
    std::array<std::uint8_t, kChunk> chunk{};
    std::uint64_t done = 0;
    while (done < count) {
        const std::size_t n =
            static_cast<std::size_t>((count - done) < kChunk ? (count - done) : kChunk);
        const ssize_t got = ::getrandom(chunk.data(), n, 0);
        if (got < 0) {
            return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
        }
        if (got == 0) {
            break;
        }
        if (!memory.write(buf + done, chunk.data(), static_cast<std::size_t>(got))) {
            return -kEfault;
        }
        done += static_cast<std::uint64_t>(got);
        if (static_cast<std::size_t>(got) < n) {
            break; // a short fill is the result, as with getrandom(2)
        }
    }
    return static_cast<std::int64_t>(done);
}

// getcwd(buf, size): copy the host working directory into the guest buffer.
// Follows getcwd(2): a buffer too small for the path plus its NUL is -ERANGE,
// and a guest pointer the memory model rejects is -EFAULT.
std::int64_t do_getcwd(GuestState& s, Memory& memory) {
    const std::uint64_t buf = arg(s, 0);
    const std::uint64_t size = arg(s, 1);
    if (size == 0) {
        return -kEinval;
    }
    std::array<char, 4096> path{};
    if (::getcwd(path.data(), path.size()) == nullptr) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    const std::size_t len = std::strlen(path.data());
    if (len + 1 > size) {
        return -kErange; // path plus NUL does not fit
    }
    if (!memory.write(buf, reinterpret_cast<const std::uint8_t*>(path.data()), len + 1)) {
        return -kEfault;
    }
    return static_cast<std::int64_t>(len + 1);
}

} // namespace

MinimalSyscallHandler::MinimalSyscallHandler(OutputSink sink, GuestAddressSpace* address_space)
    : sink_(sink ? std::move(sink) : OutputSink(default_sink)), address_space_(address_space) {
    // The standard descriptors exist from the start, as they do for a real
    // process: 0 = stdin, 1 = stdout, 2 = stderr. `write` still routes 1/2 to the
    // sink; `read`/`lseek` use the stream table, so a guest that reads fd 0 gets
    // the process's stdin (which a test can replace with adopt()).
    files_ = {GuestDescriptor{stdin, false}, GuestDescriptor{stdout, false},
              GuestDescriptor{stderr, false}};
}

bool MinimalSyscallHandler::adopt(int guest_fd, std::FILE* stream) {
    if (guest_fd < 0 || stream == nullptr) {
        return false;
    }
    const auto idx = static_cast<std::size_t>(guest_fd);
    if (idx < files_.size() && files_[idx].stream != nullptr) {
        return false; // already occupied
    }
    if (idx >= files_.size()) {
        files_.resize(idx + 1);
    }
    files_[idx] = GuestDescriptor{stream, /*owned=*/false};
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
    case sysno::kOpenat: {
        const std::int64_t r = do_openat(state, memory, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        // -ENOSYS means the request (a non-AT_FDCWD dirfd, or an unhandled flag)
        // is outside this increment: report it unimplemented so it is counted,
        // rather than as a handled call that happens to return an error.
        result.status = (r == -kEnosys) ? SyscallStatus::kUnimplemented : SyscallStatus::kHandled;
        return result;
    }
    case sysno::kFstat: {
        const std::int64_t r = do_fstat(state, memory, files_);
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
    case sysno::kGetpid:
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::getpid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGettid:
        // Single-threaded today: the guest thread id equals the host thread id.
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::gettid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGetuid:
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::getuid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGeteuid:
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::geteuid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGetgid:
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::getgid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGetegid:
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::getegid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGetppid:
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(::getppid());
        result.status = SyscallStatus::kHandled;
        return result;
    case sysno::kGetcwd: {
        const std::int64_t r = do_getcwd(state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kGetrandom: {
        const std::int64_t r = do_getrandom(state, memory);
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
           number == sysno::kFstat || number == sysno::kLseek || number == sysno::kExit ||
           number == sysno::kExitGroup || number == sysno::kMmap || number == sysno::kMprotect ||
           number == sysno::kMunmap || number == sysno::kBrk || number == sysno::kOpenat ||
           number == sysno::kGetpid || number == sysno::kGettid || number == sysno::kGetuid ||
           number == sysno::kGeteuid || number == sysno::kGetgid || number == sysno::kGetegid ||
           number == sysno::kGetppid || number == sysno::kGetcwd || number == sysno::kGetrandom;
}

} // namespace hyperion
