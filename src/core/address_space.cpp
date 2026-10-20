// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest virtual address space (Phase 6, increment 2). See address_space.hpp for
// the design rules and the security boundary this file implements.
#include "hyperion/address_space.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace hyperion {

namespace {
constexpr std::uint64_t kAlign = GuestAddressSpace::kPageSize;
} // namespace

std::optional<std::uint64_t> GuestAddressSpace::page_align_up(std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - (kAlign - 1)) {
        return std::nullopt; // would overflow
    }
    return (value + (kAlign - 1)) & ~GuestAddressSpace::kPageMask;
}

GuestAddressSpace::GuestAddressSpace(std::uint64_t mmap_base, std::uint64_t heap_base,
                                     std::uint64_t heap_capacity) {
    mmap_cursor_ = mmap_base;
    heap_base_ = heap_base;
    heap_end_ = heap_base + heap_capacity;
    brk_ = heap_base;
}

long GuestAddressSpace::find_region(std::uint64_t address) const {
    // Regions are sorted by base and non-overlapping; binary search for the last
    // region whose base is <= address.
    const auto it =
        std::upper_bound(regions_.begin(), regions_.end(), address,
                         [](std::uint64_t addr, const Region& r) { return addr < r.base; });
    if (it == regions_.begin()) {
        return -1;
    }
    const long index = static_cast<long>(std::distance(regions_.begin(), it)) - 1;
    const Region& r = regions_[static_cast<std::size_t>(index)];
    return (address < r.end()) ? index : -1;
}

bool GuestAddressSpace::range_is_free(std::uint64_t addr, std::uint64_t size) const {
    const std::uint64_t end = addr + size;
    for (const Region& r : regions_) {
        if (addr < r.end() && r.base < end) {
            return false; // overlap
        }
    }
    return true;
}

bool GuestAddressSpace::map_image(std::uint64_t base, const void* data, std::size_t size,
                                  MemPerm perm) {
    if ((base & kPageMask) != 0) {
        return false;
    }
    const auto mapped_size = page_align_up(size == 0 ? 1 : size);
    if (!mapped_size) {
        return false;
    }
    if (base > std::numeric_limits<std::uint64_t>::max() - *mapped_size) {
        return false;
    }
    if (!range_is_free(base, *mapped_size)) {
        return false;
    }

    Region region;
    region.base = base;
    region.size = *mapped_size;
    region.perm = perm;
    region.bytes.assign(static_cast<std::size_t>(*mapped_size), 0);
    if (data != nullptr && size > 0) {
        std::memcpy(region.bytes.data(), data, size);
    }

    regions_.push_back(std::move(region));
    std::sort(regions_.begin(), regions_.end(),
              [](const Region& a, const Region& b) { return a.base < b.base; });
    return true;
}

std::optional<std::uint64_t> GuestAddressSpace::map_anonymous(std::size_t size, MemPerm perm,
                                                              std::uint64_t hint) {
    if (size == 0) {
        return std::nullopt;
    }
    const auto mapped_size = page_align_up(size);
    if (!mapped_size) {
        return std::nullopt;
    }

    std::uint64_t base = 0;
    if (hint != 0 && (hint & kPageMask) == 0 && range_is_free(hint, *mapped_size) &&
        hint <= std::numeric_limits<std::uint64_t>::max() - *mapped_size) {
        base = hint;
    } else {
        // First fit at or above the cursor.
        std::uint64_t candidate = mmap_cursor_;
        while (candidate <= std::numeric_limits<std::uint64_t>::max() - *mapped_size) {
            if (range_is_free(candidate, *mapped_size)) {
                base = candidate;
                break;
            }
            const long idx = find_region(candidate);
            if (idx < 0) {
                break; // should not happen while range_is_free failed
            }
            candidate = regions_[static_cast<std::size_t>(idx)].end();
        }
        if (base == 0) {
            return std::nullopt; // no space found
        }
    }

    Region region;
    region.base = base;
    region.size = *mapped_size;
    region.perm = perm;
    region.bytes.assign(static_cast<std::size_t>(*mapped_size), 0);
    regions_.push_back(std::move(region));
    std::sort(regions_.begin(), regions_.end(),
              [](const Region& a, const Region& b) { return a.base < b.base; });
    if (base + *mapped_size > mmap_cursor_) {
        mmap_cursor_ = base + *mapped_size;
    }
    return base;
}

bool GuestAddressSpace::unmap(std::uint64_t addr, std::size_t size) {
    if (size == 0 || (addr & kPageMask) != 0) {
        return false;
    }
    const auto mapped_size = page_align_up(size);
    if (!mapped_size || addr > std::numeric_limits<std::uint64_t>::max() - *mapped_size) {
        return false;
    }

    // The whole range must be mapped with no gaps; otherwise the call fails
    // (the kernel unmaps partial ranges, but partial unmapping is not needed by
    // this increment and pretending to support it would be misleading).
    std::uint64_t cursor = addr;
    const std::uint64_t end = addr + *mapped_size;
    while (cursor < end) {
        const long idx = find_region(cursor);
        if (idx < 0) {
            return false;
        }
        const Region& r = regions_[static_cast<std::size_t>(idx)];
        if (r.base != cursor) {
            return false; // a gap or a region that starts before the request
        }
        if (r.end() > end) {
            return false; // would split a region; not supported yet
        }
        cursor = r.end();
    }
    if (cursor != end) {
        return false;
    }

    regions_.erase(
        std::remove_if(regions_.begin(), regions_.end(),
                       [&](const Region& r) { return r.base >= addr && r.end() <= end; }),
        regions_.end());
    return true;
}

bool GuestAddressSpace::protect(std::uint64_t addr, std::size_t size, MemPerm perm) {
    if (size == 0 || (addr & kPageMask) != 0) {
        return false;
    }
    const auto mapped_size = page_align_up(size);
    if (!mapped_size || addr > std::numeric_limits<std::uint64_t>::max() - *mapped_size) {
        return false;
    }
    const std::uint64_t end = addr + *mapped_size;

    // Every page in the range must be mapped; a hole makes the call fail.
    std::uint64_t cursor = addr;
    while (cursor < end) {
        const long idx = find_region(cursor);
        if (idx < 0) {
            return false;
        }
        const Region& r = regions_[static_cast<std::size_t>(idx)];
        if (r.end() < end && r.end() > cursor) {
            cursor = r.end();
            continue;
        }
        cursor = std::min(r.end(), end);
    }

    // Apply the permission to every region the range touches.
    for (Region& r : regions_) {
        if (addr < r.end() && r.base < end) {
            r.perm = perm;
        }
    }
    return true;
}

std::uint64_t GuestAddressSpace::set_brk(std::uint64_t requested) {
    if (requested < heap_base_ || requested > heap_end_) {
        return brk_; // unchanged: the request is outside the heap area
    }
    if (requested > brk_) {
        ensure_heap_mapped(requested); // growing: back the new pages
    }
    brk_ = requested;
    return brk_;
}

void GuestAddressSpace::ensure_heap_mapped(std::uint64_t upto) {
    const auto aligned = page_align_up(upto);
    if (!aligned) {
        return;
    }
    // Find an existing region anchored at the heap base, if any.
    const long idx = find_region(heap_base_);
    if (idx >= 0) {
        Region& heap = regions_[static_cast<std::size_t>(idx)];
        if (heap.base == heap_base_ && heap.end() < *aligned) {
            heap.bytes.resize(static_cast<std::size_t>(*aligned - heap.base), 0);
            heap.size = *aligned - heap.base;
        }
        return;
    }
    Region heap;
    heap.base = heap_base_;
    heap.size = *aligned - heap_base_;
    heap.perm = MemPerm::kRead | MemPerm::kWrite;
    heap.bytes.assign(static_cast<std::size_t>(heap.size), 0);
    regions_.push_back(std::move(heap));
    std::sort(regions_.begin(), regions_.end(),
              [](const Region& a, const Region& b) { return a.base < b.base; });
}

std::optional<MemPerm> GuestAddressSpace::perm_at(std::uint64_t address) const {
    const long idx = find_region(address);
    if (idx < 0) {
        return std::nullopt;
    }
    return regions_[static_cast<std::size_t>(idx)].perm;
}

bool GuestAddressSpace::access(std::uint64_t address, std::uint8_t* buf, std::size_t count,
                               bool is_write) {
    if (count == 0) {
        return true;
    }
    if (address > std::numeric_limits<std::uint64_t>::max() - count) {
        return false; // wrap-around
    }
    const std::uint64_t end = address + count;
    const MemPerm need = is_write ? MemPerm::kWrite : MemPerm::kRead;

    std::uint64_t cursor = address;
    while (cursor < end) {
        const long idx = find_region(cursor);
        if (idx < 0) {
            return false; // unmapped
        }
        Region& r = regions_[static_cast<std::size_t>(idx)];
        if (!has_perm(r.perm, need)) {
            return false; // mapped but not accessible this way
        }
        const std::uint64_t stop = std::min(r.end(), end);
        const std::size_t offset = static_cast<std::size_t>(cursor - r.base);
        const std::size_t n = static_cast<std::size_t>(stop - cursor);
        if (is_write) {
            std::memcpy(r.bytes.data() + offset, buf + (cursor - address), n);
        } else {
            std::memcpy(buf + (cursor - address), r.bytes.data() + offset, n);
        }
        cursor = stop;
    }
    return true;
}

bool GuestAddressSpace::read(std::uint64_t address, std::uint8_t* out, std::size_t count) {
    return access(address, out, count, /*is_write=*/false);
}

bool GuestAddressSpace::write(std::uint64_t address, const std::uint8_t* in, std::size_t count) {
    return access(address, const_cast<std::uint8_t*>(in), count, /*is_write=*/true);
}

} // namespace hyperion
