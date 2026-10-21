// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/exec_mem.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

namespace hyperion {

namespace {

std::size_t page_size() {
    const long p = ::sysconf(_SC_PAGESIZE);
    return p > 0 ? static_cast<std::size_t>(p) : 4096;
}

std::size_t round_up(std::size_t value, std::size_t multiple) {
    if (value > static_cast<std::size_t>(-1) - (multiple - 1)) {
        throw std::out_of_range("ExecMem: requested size overflows address calculation");
    }
    return ((value + multiple - 1) / multiple) * multiple;
}

} // namespace

ExecMem::ExecMem(std::size_t bytes) {
    size_ = round_up(bytes == 0 ? 1 : bytes, page_size());
    // PROT_READ|PROT_WRITE now; PROT_EXEC is added in finalize(). Asking for
    // RWX up front would defeat W^X and is unnecessary.
    void* p = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        throw std::runtime_error("ExecMem: mmap failed");
    }
    base_ = p;
}

ExecMem::~ExecMem() {
    release();
}

ExecMem::ExecMem(ExecMem&& other) noexcept
    : base_(other.base_), size_(other.size_), finalized_(other.finalized_) {
    other.base_ = nullptr;
    other.size_ = 0;
    other.finalized_ = false;
}

ExecMem& ExecMem::operator=(ExecMem&& other) noexcept {
    if (this != &other) {
        release();
        base_ = other.base_;
        size_ = other.size_;
        finalized_ = other.finalized_;
        other.base_ = nullptr;
        other.size_ = 0;
        other.finalized_ = false;
    }
    return *this;
}

void ExecMem::release() noexcept {
    if (base_ != nullptr) {
        ::munmap(base_, size_);
        base_ = nullptr;
        size_ = 0;
    }
}

std::uint8_t* ExecMem::writable() {
    if (finalized_) {
        throw std::logic_error("ExecMem: region is finalized (not writable)");
    }
    return static_cast<std::uint8_t*>(base_);
}

void ExecMem::write(std::size_t offset, const std::uint8_t* bytes, std::size_t count) {
    if (finalized_) {
        throw std::logic_error("ExecMem: cannot write after finalize()");
    }
    if (offset > size_ || count > size_ - offset) {
        throw std::out_of_range("ExecMem: write past end of region");
    }
    if (count > 0) {
        std::memcpy(writable() + offset, bytes, count);
    }
}

void ExecMem::finalize() {
    if (finalized_) {
        return;
    }
    if (::mprotect(base_, size_, PROT_READ | PROT_EXEC) != 0) {
        throw std::runtime_error("ExecMem: mprotect(RX) failed");
    }
    // Flush the instruction cache for the region. On AArch64 this is required
    // for correctness; on x86 it is a no-op but harmless.
    __builtin___clear_cache(static_cast<char*>(base_), static_cast<char*>(base_) + size_);
    finalized_ = true;
}

void* ExecMem::entry() const {
    if (!finalized_) {
        throw std::logic_error("ExecMem: entry() requires finalize() first");
    }
    return base_;
}

bool ExecMem::executable_supported() {
#if defined(__aarch64__)
    // Probe once whether this host process can actually obtain an RX mapping
    // under the active kernel/SELinux/seccomp policy (e.g. Android untrusted
    // app domains may deny PROT_EXEC on anonymous mappings).
    static const bool supported = [] {
        const std::size_t ps = page_size();
        void* p = ::mmap(nullptr, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            return false;
        }
        const bool ok = (::mprotect(p, ps, PROT_READ | PROT_EXEC) == 0);
        ::munmap(p, ps);
        return ok;
    }();
    return supported;
#else
    return false;
#endif
}

} // namespace hyperion
