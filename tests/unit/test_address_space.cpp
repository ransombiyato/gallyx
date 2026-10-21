// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Guest address space tests (Phase 6, increment 2).
//
// These verify the security-relevant behaviour of GuestAddressSpace: that every
// access is bounds- and permission-checked, that ranges cannot wrap around the
// address space, and that map/unmap/protect/brk follow the kernel's contract.
// The class is exercised through the same Memory interface the interpreter and
// translator use, so the tests also serve as an integration check.
#include <cstdint>
#include <cstring>
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/address_space.hpp"

using hyperion::GuestAddressSpace;
using hyperion::MemPerm;

namespace {

constexpr std::uint64_t kPage = GuestAddressSpace::kPageSize;

} // namespace

HYPERION_TEST("address_space", "map_image_is_readable_and_reports_its_permissions") {
    GuestAddressSpace as;
    const std::uint8_t image[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    HYPERION_CHECK(as.map_image(0x400000, image, sizeof(image), MemPerm::kRead | MemPerm::kExec));

    std::uint8_t out[4] = {};
    HYPERION_CHECK(as.read(0x400000, out, sizeof(out)));
    HYPERION_CHECK(out[0] == 0xDE && out[3] == 0xEF);

    const auto perm = as.perm_at(0x400000);
    HYPERION_CHECK(perm.has_value());
    HYPERION_CHECK(hyperion::has_perm(*perm, MemPerm::kRead));
    HYPERION_CHECK(hyperion::has_perm(*perm, MemPerm::kExec));
    HYPERION_CHECK(!hyperion::has_perm(*perm, MemPerm::kWrite));
}

HYPERION_TEST("address_space", "write_to_read_only_mapping_fails") {
    GuestAddressSpace as;
    const std::uint8_t image[4] = {1, 2, 3, 4};
    HYPERION_CHECK(as.map_image(0x400000, image, sizeof(image), MemPerm::kRead));
    const std::uint8_t value = 0xFF;
    HYPERION_CHECK(!as.write(0x400000, &value, 1)); // read-only: must fault
    std::uint8_t out = 0;
    HYPERION_CHECK(as.read(0x400000, &out, 1)); // still readable
    HYPERION_CHECK(out == 1);
}

HYPERION_TEST("address_space", "read_past_the_end_of_a_mapping_faults") {
    GuestAddressSpace as;
    const std::uint8_t image[4] = {1, 2, 3, 4};
    HYPERION_CHECK(as.map_image(0x400000, image, sizeof(image), MemPerm::kRead));
    std::uint8_t out[4] = {};
    // The image occupies one page at 0x400000; a read starting inside it but
    // crossing the page boundary must fail rather than return neighbouring
    // bytes.
    HYPERION_CHECK(!as.read(0x400000 + kPage - 2, out, 4));
    // A read entirely within the page succeeds.
    HYPERION_CHECK(as.read(0x400000, out, 4));
}

HYPERION_TEST("address_space", "unmapped_read_and_write_fault") {
    GuestAddressSpace as;
    std::uint8_t buf[8] = {};
    HYPERION_CHECK(!as.read(0x1000, buf, sizeof(buf)));
    HYPERION_CHECK(!as.write(0x1000, buf, sizeof(buf)));
}

HYPERION_TEST("address_space", "anonymous_mapping_is_writable_and_disjoint") {
    GuestAddressSpace as;
    const auto a = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite);
    const auto b = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite);
    HYPERION_CHECK(a.has_value());
    HYPERION_CHECK(b.has_value());
    HYPERION_CHECK(*a != *b);
    HYPERION_CHECK(*b >= *a + kPage || *a >= *b + kPage); // no overlap

    const std::uint8_t value = 0x42;
    HYPERION_CHECK(as.write(*a, &value, 1));
    std::uint8_t out = 0;
    HYPERION_CHECK(as.read(*a, &out, 1));
    HYPERION_CHECK(out == 0x42);
}

HYPERION_TEST("address_space", "anonymous_mapping_honours_a_free_hint") {
    GuestAddressSpace as;
    const std::uint64_t hint = 0x600000;
    const auto got = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite, hint);
    HYPERION_CHECK(got.has_value());
    HYPERION_CHECK(*got == hint);

    // A second request at the same hint cannot use it and must fall back.
    const auto second = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite, hint);
    HYPERION_CHECK(second.has_value());
    HYPERION_CHECK(*second != hint);
}

HYPERION_TEST("address_space", "mprotect_changes_access") {
    GuestAddressSpace as;
    const auto addr = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite);
    HYPERION_CHECK(addr.has_value());
    const std::uint8_t v = 7;
    HYPERION_CHECK(as.write(*addr, &v, 1));

    HYPERION_CHECK(as.protect(*addr, kPage, MemPerm::kRead)); // drop write
    HYPERION_CHECK(!as.write(*addr, &v, 1));

    HYPERION_CHECK(as.protect(*addr, kPage, MemPerm::kNone)); // PROT_NONE
    std::uint8_t out = 0;
    HYPERION_CHECK(!as.read(*addr, &out, 1));

    HYPERION_CHECK(as.protect(*addr, kPage, MemPerm::kRead | MemPerm::kWrite)); // restore
    HYPERION_CHECK(as.read(*addr, &out, 1));
    HYPERION_CHECK(out == 7); // contents survive a permission change
}

HYPERION_TEST("address_space", "mprotect_on_an_unmapped_range_fails") {
    GuestAddressSpace as;
    HYPERION_CHECK(!as.protect(0x900000, kPage, MemPerm::kRead));
}

HYPERION_TEST("address_space", "unmap_removes_the_mapping") {
    GuestAddressSpace as;
    const auto addr = as.map_anonymous(2 * kPage, MemPerm::kRead | MemPerm::kWrite);
    HYPERION_CHECK(addr.has_value());
    const std::uint8_t v = 1;
    HYPERION_CHECK(as.write(*addr, &v, 1));

    HYPERION_CHECK(as.unmap(*addr, 2 * kPage));
    std::uint8_t out = 0;
    HYPERION_CHECK(!as.read(*addr, &out, 1));
    HYPERION_CHECK(!as.perm_at(*addr).has_value());
}

HYPERION_TEST("address_space", "unmap_of_a_partial_or_unmapped_range_fails") {
    GuestAddressSpace as;
    const auto addr = as.map_anonymous(2 * kPage, MemPerm::kRead | MemPerm::kWrite);
    HYPERION_CHECK(addr.has_value());
    // Splitting a region is not supported in this increment: it must fail
    // loudly rather than silently succeed with a different mapping.
    HYPERION_CHECK(!as.unmap(*addr, kPage));
    HYPERION_CHECK(!as.unmap(0x900000, kPage));
}

HYPERION_TEST("address_space", "overlapping_map_is_rejected") {
    GuestAddressSpace as;
    HYPERION_CHECK(as.map_image(0x400000, nullptr, kPage, MemPerm::kRead));
    HYPERION_CHECK(!as.map_image(0x400000, nullptr, kPage, MemPerm::kRead));
    HYPERION_CHECK(!as.map_image(0x400800, nullptr, kPage, MemPerm::kRead)); // straddles
}

HYPERION_TEST("address_space", "misaligned_requests_are_rejected") {
    GuestAddressSpace as;
    HYPERION_CHECK(!as.map_image(0x400001, nullptr, 16, MemPerm::kRead)); // not page-aligned
    HYPERION_CHECK(!as.unmap(1, kPage));
    HYPERION_CHECK(!as.protect(1, kPage, MemPerm::kRead));
}

HYPERION_TEST("address_space", "a_huge_size_does_not_wrap") {
    GuestAddressSpace as;
    // A guest-supplied size near UINT64_MAX must not wrap the end calculation
    // into a valid-looking range.
    std::uint8_t buf[8] = {};
    HYPERION_CHECK(!as.read(UINT64_MAX - 3, buf, 8));
    HYPERION_CHECK(!as.write(UINT64_MAX - 3, buf, 8));
    HYPERION_CHECK(!as.map_anonymous(SIZE_MAX, MemPerm::kRead));
}

HYPERION_TEST("address_space", "brk_grows_and_shrinks_within_bounds") {
    GuestAddressSpace as(0x7f0000000000ULL, 0x200000ULL, 1ULL << 20);
    HYPERION_CHECK(as.brk() == 0x200000ULL);

    // Growing within the heap area succeeds and the new memory is writable.
    HYPERION_CHECK(as.set_brk(0x201000) == 0x201000);
    const std::uint8_t v = 0x5A;
    HYPERION_CHECK(as.write(0x200FF0, &v, 1));

    // Shrinking is allowed.
    HYPERION_CHECK(as.set_brk(0x200800) == 0x200800);

    // A request outside the reserved heap area leaves brk unchanged. The heap
    // area is [0x200000, 0x300000) here.
    HYPERION_CHECK(as.set_brk(0x400000) == 0x200800);
    HYPERION_CHECK(as.set_brk(0x100000) == 0x200800);
}

HYPERION_TEST("address_space", "a_read_spanning_two_regions_succeeds_when_both_allow_it") {
    // Adjacent anonymous mappings: a read crossing the boundary must stitch
    // them together, not fail.
    GuestAddressSpace as;
    const auto a = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite, 0x100000);
    HYPERION_CHECK(a.has_value());
    const auto b = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite, 0x101000);
    HYPERION_CHECK(b.has_value());
    HYPERION_CHECK(*b == *a + kPage);

    const std::uint8_t tail = 0x11;
    const std::uint8_t head = 0x22;
    HYPERION_CHECK(as.write(*a + kPage - 1, &tail, 1));
    HYPERION_CHECK(as.write(*b, &head, 1));

    std::uint8_t out[2] = {};
    HYPERION_CHECK(as.read(*a + kPage - 1, out, 2));
    HYPERION_CHECK(out[0] == 0x11 && out[1] == 0x22);
}

HYPERION_TEST("address_space", "a_read_spanning_a_region_without_permission_fails") {
    GuestAddressSpace as;
    const auto a = as.map_anonymous(kPage, MemPerm::kRead | MemPerm::kWrite, 0x100000);
    HYPERION_CHECK(a.has_value());
    const auto b = as.map_anonymous(kPage, MemPerm::kNone, 0x101000); // PROT_NONE
    HYPERION_CHECK(b.has_value());

    std::uint8_t out[2] = {};
    HYPERION_CHECK(!as.read(*a + kPage - 1, out, 2)); // crosses into PROT_NONE
}

HYPERION_TEST("address_space", "an_oversized_anonymous_mapping_is_refused") {
    // The backing store is host memory. A single mapping larger than the cap
    // must be refused (nullopt -> ENOMEM) rather than driving an unbounded host
    // allocation. Uses a tiny cap so the test never allocates anything large.
    GuestAddressSpace as(/*mmap_base=*/0x7f0000000000ULL, /*heap_base=*/0x200000ULL,
                         /*heap_capacity=*/1ULL << 20, /*max_map_bytes=*/64 * 1024);
    HYPERION_CHECK(!as.map_anonymous(1024 * 1024, MemPerm::kRead | MemPerm::kWrite).has_value());
    // A request at or below the cap still succeeds.
    HYPERION_CHECK(as.map_anonymous(64 * 1024, MemPerm::kRead | MemPerm::kWrite).has_value());
}
