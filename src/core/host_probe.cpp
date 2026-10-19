// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/host_probe.hpp"

#include <cstdio>
#include <sstream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace hyperion {
namespace {

HostArch detect_arch() {
#if defined(__x86_64__) || defined(_M_X64)
    return HostArch::kX86_64;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return HostArch::kAarch64;
#else
    return HostArch::kUnknown;
#endif
}

std::size_t detect_cache_line() {
#if defined(__linux__)
    // sysconf is not required to provide this; fall back to 0 when absent.
    const long v = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (v > 0) {
        return static_cast<std::size_t>(v);
    }
#endif
    return 0;
}

} // namespace

const char* to_string(HostArch arch) {
    switch (arch) {
    case HostArch::kX86_64:
        return "x86_64";
    case HostArch::kAarch64:
        return "aarch64";
    case HostArch::kUnknown:
        break;
    }
    return "unknown";
}

HostInfo probe_host() {
    HostInfo info;
    info.arch = detect_arch();

#if defined(__linux__)
    info.os_name = "Linux";

    struct utsname u{};
    if (uname(&u) == 0) {
        info.kernel_release = u.release;
    }

    const long pages = sysconf(_SC_PAGESIZE);
    if (pages > 0) {
        info.page_size = static_cast<std::size_t>(pages);
    }

    // THP is reported as "always"/"madvise"/"never" with the active setting
    // bracketed; anything but "never" means large pages may appear behind
    // anonymous mappings, which affects code-cache assumptions.
    if (FILE* f = std::fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r")) {
        char buf[64] = {};
        if (std::fgets(buf, sizeof(buf), f) != nullptr) {
            const std::string s(buf);
            info.transparent_hugepages = s.find("[never]") == std::string::npos;
        }
        std::fclose(f);
    }
#endif

    unsigned hw = std::thread::hardware_concurrency();
    info.logical_cpus = hw == 0 ? 1U : hw;
    info.cache_line_size = detect_cache_line();

    // On AArch64 Linux with a modern kernel, pages are never simultaneously
    // writable and executable; the code generator must assume W^X.
    info.has_wx_violation_support = (info.arch == HostArch::kAarch64);

    return info;
}

std::string format_host_info(const HostInfo& info) {
    std::ostringstream os;
    os << "arch:            " << to_string(info.arch) << '\n';
    os << "os:              " << (info.os_name.empty() ? "unknown" : info.os_name) << '\n';
    os << "kernel:          " << (info.kernel_release.empty() ? "unknown" : info.kernel_release)
       << '\n';
    os << "logical cpus:    " << info.logical_cpus << '\n';
    os << "page size:       " << info.page_size << " bytes\n";
    os << "cache line:      "
       << (info.cache_line_size ? std::to_string(info.cache_line_size) + " bytes" : "unknown")
       << '\n';
    os << "transparent hp:  " << (info.transparent_hugepages ? "enabled" : "disabled") << '\n';
    os << "assume W^X:      " << (info.has_wx_violation_support ? "yes" : "no") << '\n';
    return os.str();
}

} // namespace hyperion
