// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Tests for the host capability probe.
//
// These tests assert *invariants of the probe itself* (it never fabricates
// data, it agrees with the compile-time architecture) rather than asserting
// properties of a particular machine, so they pass on both x86-64 and AArch64.
#include "harness/test_harness.hpp"
#include "hyperion/host_probe.hpp"

#include <string>

using hyperion::HostArch;
using hyperion::HostInfo;
using hyperion::probe_host;
using hyperion::to_string;

HYPERION_TEST("host_probe", "arch_matches_compile_time_arch") {
    const HostInfo info = probe_host();
#if defined(__x86_64__)
    HYPERION_CHECK(info.arch == HostArch::kX86_64);
#elif defined(__aarch64__)
    HYPERION_CHECK(info.arch == HostArch::kAarch64);
#endif
    HYPERION_CHECK(std::string(to_string(info.arch)) != "unknown");
}

HYPERION_TEST("host_probe", "reports_plausible_cpu_and_page_size") {
    const HostInfo info = probe_host();
    HYPERION_CHECK(info.logical_cpus >= 1U);
    // Page size is always a power of two on supported systems.
    HYPERION_CHECK(info.page_size >= 4096U);
    HYPERION_CHECK((info.page_size & (info.page_size - 1U)) == 0U);
}

HYPERION_TEST("host_probe", "wx_assumption_follows_architecture") {
    const HostInfo info = probe_host();
    // The translator must assume W^X on AArch64 and not on x86-64.
    if (info.arch == HostArch::kAarch64) {
        HYPERION_CHECK(info.has_wx_violation_support);
    } else if (info.arch == HostArch::kX86_64) {
        HYPERION_CHECK(!info.has_wx_violation_support);
    }
}

HYPERION_TEST("host_probe", "format_includes_every_measured_field") {
    const HostInfo info = probe_host();
    const std::string report = hyperion::format_host_info(info);
    HYPERION_CHECK(report.find("arch:") != std::string::npos);
    HYPERION_CHECK(report.find("page size:") != std::string::npos);
    HYPERION_CHECK(report.find(to_string(info.arch)) != std::string::npos);
}

HYPERION_TEST("host_probe", "unknown_arch_string_is_stable") {
    HYPERION_CHECK_EQ(std::string(to_string(HostArch::kUnknown)), std::string("unknown"));
}
