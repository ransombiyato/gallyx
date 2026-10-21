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
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace hyperion {

OpenFileDescription::~OpenFileDescription() {
    if (owned_fd && host_fd >= 0) {
        ::close(host_fd);
        host_fd = -1;
    }
}

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

// On Linux x86-64, kernel syscall wrappers cast C `int` arguments (`fd`,
// `dirfd`, etc.) to 32-bit signed `int`, ignoring the upper 32 bits of the
// register. Without this truncation, `mov edi, -100` (`RDI = 0x00000000ffffff9c`)
// fails a 64-bit comparison against `-100` (`0xffffffffffffff9c`).
std::int32_t arg_i32(const GuestState& s, int index) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(arg(s, index)));
}

constexpr std::int32_t kGuestAtFdcwd = -100;

long default_sink(int fd, const std::uint8_t* data, std::size_t count) {
    std::FILE* out = (fd == 2) ? stderr : stdout;
    const std::size_t wrote = std::fwrite(data, 1, count, out);
    std::fflush(out);
    return static_cast<long>(wrote);
}

// Look up an open guest descriptor slot in the table. Returns nullptr for a
// negative, out-of-range, or closed descriptor; callers map that to -EBADF.
const GuestDescriptor* lookup_fd(const std::vector<GuestDescriptor>& files, std::int32_t fd) {
    if (fd < 0) {
        return nullptr;
    }
    const auto idx = static_cast<std::size_t>(fd);
    if (idx >= files.size() || !files[idx].is_open()) {
        return nullptr;
    }
    return &files[idx];
}

const OpenFileDescription* lookup_ofd(const std::vector<GuestDescriptor>& files, std::int32_t fd) {
    const GuestDescriptor* e = lookup_fd(files, fd);
    return e == nullptr ? nullptr : e->desc.get();
}

int underlying_host_fd(const OpenFileDescription& ofd) {
    if (ofd.host_fd >= 0) {
        return ofd.host_fd;
    }
    if (ofd.stream != nullptr) {
        return ::fileno(ofd.stream);
    }
    return -1;
}

// Allocate the lowest free guest descriptor >= min_fd, as the kernel does.
std::int64_t alloc_fd(std::vector<GuestDescriptor>& files,
                      std::shared_ptr<OpenFileDescription> desc, bool cloexec,
                      std::size_t min_fd = 3) {
    constexpr std::size_t kMaxGuestFds = 4096;
    if (min_fd >= kMaxGuestFds) {
        return -kEinval;
    }
    for (std::size_t i = min_fd; i < files.size(); ++i) {
        if (!files[i].is_open()) {
            files[i] = GuestDescriptor{std::move(desc), cloexec};
            return static_cast<std::int64_t>(i);
        }
    }
    if (files.size() >= kMaxGuestFds) {
        return -kEmfile;
    }
    if (files.size() < min_fd) {
        files.resize(min_fd);
    }
    files.push_back(GuestDescriptor{std::move(desc), cloexec});
    return static_cast<std::int64_t>(files.size() - 1);
}

// write(fd, buf, count): write `count` guest bytes through an open guest
// descriptor (sink-routed stdout/stderr, unbuffered kernel file descriptor
// opened via openat, or an adopted host FILE*).
std::int64_t do_write(GuestState& s, Memory& memory, const MinimalSyscallHandler::OutputSink& sink,
                      const std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const std::uint64_t buf = arg(s, 1);
    const std::uint64_t count = arg(s, 2);

    const OpenFileDescription* ofd = lookup_ofd(files, fd);
    if (ofd == nullptr) {
        return -kEbadf;
    }
    if (count == 0) {
        return 0;
    }

    constexpr std::size_t kChunk = 4096;
    std::array<std::uint8_t, kChunk> chunk{};
    std::uint64_t done = 0;
    while (done < count) {
        const std::size_t n =
            static_cast<std::size_t>((count - done) < kChunk ? (count - done) : kChunk);
        if (!memory.read(buf + done, chunk.data(), n)) {
            return done > 0 ? static_cast<std::int64_t>(done) : -kEfault;
        }
        if (ofd->sink_fd >= 0) {
            if (sink) {
                const long wrote = sink(ofd->sink_fd, chunk.data(), n);
                if (wrote < 0) {
                    return done > 0 ? static_cast<std::int64_t>(done) : -kEbadf;
                }
                done += static_cast<std::uint64_t>(wrote);
                if (static_cast<std::size_t>(wrote) < n) {
                    break;
                }
            } else {
                done += n;
            }
        } else if (ofd->host_fd >= 0) {
            const ssize_t wrote = ::write(ofd->host_fd, chunk.data(), n);
            if (wrote < 0) {
                const std::int64_t err = -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
                return done > 0 ? static_cast<std::int64_t>(done) : err;
            }
            done += static_cast<std::uint64_t>(wrote);
            if (static_cast<std::size_t>(wrote) < n) {
                break;
            }
        } else if (ofd->stream != nullptr) {
            const std::size_t wrote = std::fwrite(chunk.data(), 1, n, ofd->stream);
            if (wrote == 0) {
                return done > 0 ? static_cast<std::int64_t>(done) : -kEbadf;
            }
            std::fflush(ofd->stream);
            done += wrote;
            if (wrote < n) {
                break;
            }
        } else {
            return -kEbadf;
        }
    }
    return static_cast<std::int64_t>(done);
}

// read(fd, buf, count): read up to `count` bytes from an open guest descriptor
// into guest memory.
std::int64_t do_read(GuestState& s, Memory& memory, const std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const std::uint64_t buf = arg(s, 1);
    const std::uint64_t count = arg(s, 2);

    const OpenFileDescription* ofd = lookup_ofd(files, fd);
    if (ofd == nullptr) {
        return -kEbadf;
    }
    if (count == 0) {
        return 0;
    }

    constexpr std::size_t kChunk = 4096;
    std::array<std::uint8_t, kChunk> chunk{};
    std::uint64_t done = 0;
    while (done < count) {
        const std::size_t n =
            static_cast<std::size_t>((count - done) < kChunk ? (count - done) : kChunk);
        std::size_t got = 0;
        if (ofd->host_fd >= 0) {
            const ssize_t rc = ::read(ofd->host_fd, chunk.data(), n);
            if (rc < 0) {
                const std::int64_t err = -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
                return done > 0 ? static_cast<std::int64_t>(done) : err;
            }
            got = static_cast<std::size_t>(rc);
        } else if (ofd->stream != nullptr) {
            got = std::fread(chunk.data(), 1, n, ofd->stream);
        } else {
            return -kEbadf;
        }
        if (got == 0) {
            break; // EOF
        }
        if (!memory.write(buf + done, chunk.data(), got)) {
            return -kEfault;
        }
        done += got;
        if (got < n) {
            break;
        }
    }
    return static_cast<std::int64_t>(done);
}

// getdents64(fd, dirp, count): read directory entries into the guest buffer.
std::int64_t do_getdents64(GuestState& s, Memory& memory,
                           const std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const std::uint64_t buf = arg(s, 1);
    const std::uint64_t count = arg(s, 2);

    const OpenFileDescription* ofd = lookup_ofd(files, fd);
    if (ofd == nullptr) {
        return -kEbadf;
    }
    const int host_fd = underlying_host_fd(*ofd);
    if (host_fd < 0) {
        return -kEnotdir;
    }
    constexpr std::uint64_t kDirent64Size = 24;
    if (count < kDirent64Size) {
        return -kEinval;
    }
    constexpr std::size_t kMaxGetdents = 8192;
    const std::size_t want = static_cast<std::size_t>(count < kMaxGetdents ? count : kMaxGetdents);
    std::array<std::uint8_t, kMaxGetdents> entries{};
    const long got = ::syscall(SYS_getdents64, host_fd, entries.data(), want);
    if (got < 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    if (!memory.write(buf, entries.data(), static_cast<std::size_t>(got))) {
        return -kEfault;
    }
    return static_cast<std::int64_t>(got);
}

// close(fd): release one descriptor table slot. The underlying OpenFileDescription
// closes `host_fd` automatically when the last duplicated slot drops its reference.
std::int64_t do_close(GuestState& s, std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const GuestDescriptor* e = lookup_fd(files, fd);
    if (e == nullptr) {
        return -kEbadf;
    }
    files[static_cast<std::size_t>(fd)] = GuestDescriptor{};
    return 0;
}

// lseek(fd, offset, whence): reposition an open file description.
std::int64_t do_lseek(GuestState& s, const std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const auto offset = static_cast<std::int64_t>(arg(s, 1));
    const auto whence = static_cast<std::uint32_t>(arg(s, 2));

    const OpenFileDescription* ofd = lookup_ofd(files, fd);
    if (ofd == nullptr) {
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
    if (ofd->host_fd >= 0) {
        const off_t pos = ::lseek(ofd->host_fd, static_cast<off_t>(offset), host_whence);
        if (pos < 0) {
            return -static_cast<std::int64_t>(errno == 0 ? kEspipe : errno);
        }
        return static_cast<std::int64_t>(pos);
    }
    if (ofd->stream != nullptr) {
        errno = 0;
        if (std::fseek(ofd->stream, static_cast<long>(offset), host_whence) != 0) {
            return -static_cast<std::int64_t>(errno == EINVAL ? kEinval : kEspipe);
        }
        const long pos = std::ftell(ofd->stream);
        if (pos < 0) {
            return -kEinval;
        }
        return static_cast<std::int64_t>(pos);
    }
    return -kEspipe;
}

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
        return {0, false};
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
    if ((flags & open_flags::kODirectory) != 0) {
        r.host |= O_DIRECTORY;
    }
    if ((flags & open_flags::kONonblock) != 0) {
        r.host |= O_NONBLOCK;
    }
    constexpr std::uint64_t kHonoured = open_flags::kOAccmode | open_flags::kOCreat |
                                        open_flags::kOTrunc | open_flags::kOAppend |
                                        open_flags::kOExcl | open_flags::kODirectory |
                                        open_flags::kONonblock | open_flags::kOCloseexec;
    if ((flags & ~kHonoured) != 0) {
        return {0, false};
    }
    return r;
}

std::int64_t do_openat(GuestState& s, Memory& memory, std::vector<GuestDescriptor>& files) {
    const std::int32_t dirfd = arg_i32(s, 0);
    const std::uint64_t path_addr = arg(s, 1);
    const auto flags = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 2)));
    const auto mode = static_cast<mode_t>(arg(s, 3) & 0xFFFFu);

    if (dirfd != kGuestAtFdcwd) {
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

    const int host_fd = ::open(path.c_str(), of.host | O_CLOEXEC, mode);
    if (host_fd < 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    auto desc = std::make_shared<OpenFileDescription>();
    desc->host_fd = host_fd;
    desc->owned_fd = true;
    desc->status_flags = of.host;
    const bool cloexec = (flags & open_flags::kOCloseexec) != 0;
    return alloc_fd(files, std::move(desc), cloexec);
}

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

std::int64_t write_guest_stat(Memory& memory, std::uint64_t buf, const struct stat& st) {
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

std::int64_t do_fstat(GuestState& s, Memory& memory, const std::vector<GuestDescriptor>& files) {
    const std::int32_t fd = arg_i32(s, 0);
    const std::uint64_t buf = arg(s, 1);

    const OpenFileDescription* ofd = lookup_ofd(files, fd);
    if (ofd == nullptr) {
        return -kEbadf;
    }
    const int host_fd = underlying_host_fd(*ofd);
    if (host_fd < 0) {
        return -kEbadf;
    }
    struct stat st{};
    if (::fstat(host_fd, &st) != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    return write_guest_stat(memory, buf, st);
}

std::int64_t do_newfstatat(GuestState& s, Memory& memory) {
    const std::int32_t dirfd = arg_i32(s, 0);
    const std::uint64_t path_addr = arg(s, 1);
    const std::uint64_t buf = arg(s, 2);
    const auto flags = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 3)));

    if (dirfd != kGuestAtFdcwd) {
        return -kEnosys;
    }
    if ((flags & ~kAtSymlinkNofollow) != 0) {
        return -kEnosys;
    }
    std::string path;
    const std::int64_t rc = read_guest_cstr(memory, path_addr, path);
    if (rc != 0) {
        return rc;
    }
    struct stat st{};
    const int host_rc =
        (flags & kAtSymlinkNofollow) != 0 ? ::lstat(path.c_str(), &st) : ::stat(path.c_str(), &st);
    if (host_rc != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    return write_guest_stat(memory, buf, st);
}

std::int64_t do_statx(GuestState& s, Memory& memory) {
    const std::int32_t dirfd = arg_i32(s, 0);
    const std::uint64_t path_addr = arg(s, 1);
    const auto flags = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 2)));
    const auto mask = static_cast<unsigned>(arg(s, 3));
    const std::uint64_t buf = arg(s, 4);

    if (dirfd != kGuestAtFdcwd) {
        return -kEnosys;
    }
    if ((flags & ~kAtSymlinkNofollow) != 0) {
        return -kEnosys;
    }
    std::string path;
    const std::int64_t rc = read_guest_cstr(memory, path_addr, path);
    if (rc != 0) {
        return rc;
    }
    std::array<std::uint8_t, guest_statx::kSize> out{};
    const long host_rc = ::syscall(SYS_statx, static_cast<int>(dirfd), path.c_str(),
                                   static_cast<int>(flags), mask, out.data());
    if (host_rc != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    if (!memory.write(buf, out.data(), out.size())) {
        return -kEfault;
    }
    return 0;
}

std::int64_t readlink_common(Memory& memory, std::uint64_t path_addr, std::uint64_t buf,
                             std::uint64_t bufsiz) {
    if (bufsiz == 0) {
        return -kEinval;
    }
    std::string path;
    const std::int64_t rc = read_guest_cstr(memory, path_addr, path);
    if (rc != 0) {
        return rc;
    }
    constexpr std::size_t kMaxTarget = 4096;
    std::array<char, kMaxTarget> target{};
    const std::size_t want = static_cast<std::size_t>(bufsiz < kMaxTarget ? bufsiz : kMaxTarget);
    const ssize_t got = ::readlink(path.c_str(), target.data(), want);
    if (got < 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    if (!memory.write(buf, reinterpret_cast<const std::uint8_t*>(target.data()),
                      static_cast<std::size_t>(got))) {
        return -kEfault;
    }
    return static_cast<std::int64_t>(got);
}

std::int64_t do_readlink(GuestState& s, Memory& memory) {
    return readlink_common(memory, arg(s, 0), arg(s, 1), arg(s, 2));
}

std::int64_t do_readlinkat(GuestState& s, Memory& memory) {
    const std::int32_t dirfd = arg_i32(s, 0);
    if (dirfd != kGuestAtFdcwd) {
        return -kEnosys;
    }
    return readlink_common(memory, arg(s, 1), arg(s, 2), arg(s, 3));
}

std::int64_t do_access(GuestState& s, Memory& memory) {
    const std::uint64_t path_addr = arg(s, 0);
    const auto mode = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 1)));

    constexpr std::uint64_t kModeMask = 0x7; // R_OK|W_OK|X_OK
    if ((mode & ~kModeMask) != 0) {
        return -kEinval;
    }
    std::string path;
    const std::int64_t rc = read_guest_cstr(memory, path_addr, path);
    if (rc != 0) {
        return rc;
    }
    if (::access(path.c_str(), static_cast<int>(mode)) != 0) {
        return -static_cast<std::int64_t>(errno == 0 ? kEio : errno);
    }
    return 0;
}

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

struct MemResult {
    std::int64_t value = 0;
    bool implemented = true;
};

MemResult do_mmap(GuestState& s, GuestAddressSpace& as) {
    const std::uint64_t addr = arg(s, 0);
    const std::uint64_t length = arg(s, 1);
    const auto prot = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 2)));
    const auto flags = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 3)));

    if (length == 0) {
        return {-kEinval, true};
    }
    if ((flags & mmap_flags::kMapAnonymous) == 0) {
        return {-kEnosys, false};
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
    const auto prot = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 2)));
    if (len == 0 || (addr & GuestAddressSpace::kPageMask) != 0) {
        return {-kEinval, true};
    }
    if (!as.protect(addr, static_cast<std::size_t>(len), prot_to_perm(prot))) {
        return {-kEnomem, true};
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
        return {-kEinval, true};
    }
    return {0, true};
}

MemResult do_brk(GuestState& s, GuestAddressSpace& as) {
    const std::uint64_t requested = arg(s, 0);
    return {static_cast<std::int64_t>(as.set_brk(requested)), true};
}

std::int64_t do_getrandom(GuestState& s, Memory& memory) {
    const std::uint64_t buf = arg(s, 0);
    const std::uint64_t count = arg(s, 1);
    const auto flags = static_cast<std::uint64_t>(static_cast<std::uint32_t>(arg(s, 2)));

    constexpr std::uint64_t kHonoured = 0x1 | 0x2; // GRND_NONBLOCK | GRND_RANDOM
    if ((flags & ~kHonoured) != 0) {
        return -kEinval;
    }
    if (count == 0) {
        return 0;
    }

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
            break;
        }
    }
    return static_cast<std::int64_t>(done);
}

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
        return -kErange;
    }
    if (!memory.write(buf, reinterpret_cast<const std::uint8_t*>(path.data()), len + 1)) {
        return -kEfault;
    }
    return static_cast<std::int64_t>(len + 1);
}

std::shared_ptr<OpenFileDescription> make_stdio_desc(std::FILE* stream, int sink_fd) {
    auto d = std::make_shared<OpenFileDescription>();
    d->stream = stream;
    d->sink_fd = sink_fd;
    return d;
}

} // namespace

MinimalSyscallHandler::MinimalSyscallHandler(OutputSink sink, GuestAddressSpace* address_space)
    : sink_(sink ? std::move(sink) : OutputSink(default_sink)), address_space_(address_space) {
    files_ = {
        GuestDescriptor{make_stdio_desc(stdin, -1), false},
        GuestDescriptor{make_stdio_desc(stdout, 1), false},
        GuestDescriptor{make_stdio_desc(stderr, 2), false},
    };
}

bool MinimalSyscallHandler::adopt(int guest_fd, std::FILE* stream) {
    if (guest_fd < 0 || stream == nullptr) {
        return false;
    }
    const auto idx = static_cast<std::size_t>(guest_fd);
    if (idx < files_.size() && files_[idx].is_open()) {
        return false; // already occupied
    }
    if (idx >= files_.size()) {
        files_.resize(idx + 1);
    }
    auto desc = std::make_shared<OpenFileDescription>();
    desc->stream = stream;
    files_[idx] = GuestDescriptor{std::move(desc), false};
    return true;
}

SyscallResult MinimalSyscallHandler::handle(GuestState& state, Memory& memory) {
    SyscallResult result;
    result.number = state.reg(Gpr::kRax);

    switch (result.number) {
    case sysno::kWrite: {
        const std::int64_t r = do_write(state, memory, sink_, files_);
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
    case sysno::kGetdents64: {
        const std::int64_t r = do_getdents64(state, memory, files_);
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
        result.status = (r == -kEnosys) ? SyscallStatus::kUnimplemented : SyscallStatus::kHandled;
        return result;
    }
    case sysno::kFstat: {
        const std::int64_t r = do_fstat(state, memory, files_);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kNewfstatat: {
        const std::int64_t r = do_newfstatat(state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = (r == -kEnosys) ? SyscallStatus::kUnimplemented : SyscallStatus::kHandled;
        return result;
    }
    case sysno::kStatx: {
        const std::int64_t r = do_statx(state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = (r == -kEnosys) ? SyscallStatus::kUnimplemented : SyscallStatus::kHandled;
        return result;
    }
    case sysno::kReadlink: {
        const std::int64_t r = do_readlink(state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = SyscallStatus::kHandled;
        return result;
    }
    case sysno::kReadlinkat: {
        const std::int64_t r = do_readlinkat(state, memory);
        state.reg(Gpr::kRax) = static_cast<std::uint64_t>(r);
        result.status = (r == -kEnosys) ? SyscallStatus::kUnimplemented : SyscallStatus::kHandled;
        return result;
    }
    case sysno::kAccess: {
        const std::int64_t r = do_access(state, memory);
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
        result.exit_code = static_cast<std::int64_t>(arg_i32(state, 0)) & 0xFF;
        return result;
    case sysno::kMmap:
    case sysno::kMprotect:
    case sysno::kMunmap:
    case sysno::kBrk: {
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
           number == sysno::kNewfstatat || number == sysno::kGetpid || number == sysno::kGettid ||
           number == sysno::kGetuid || number == sysno::kGeteuid || number == sysno::kGetgid ||
           number == sysno::kGetegid || number == sysno::kGetppid || number == sysno::kGetcwd ||
           number == sysno::kGetrandom || number == sysno::kReadlink || number == sysno::kAccess ||
           number == sysno::kReadlinkat || number == sysno::kGetdents64 || number == sysno::kStatx;
}

} // namespace hyperion
