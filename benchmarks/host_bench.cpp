// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Host benchmark harness.
//
// Scope: establishes a reproducible baseline *methodology* before any guest
// execution exists. It records the measured host fingerprint and a small,
// deterministic calibration workload, then emits machine-readable JSON.
//
// It intentionally contains no guest-execution metrics: those fields are
// reported as null until the translator exists, so that reports can never be
// mistaken for fabricated performance claims.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "hyperion/host_probe.hpp"
#include "hyperion/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// Deterministic integer workload; result is compared across runs as a
// correctness guard so a benchmark cannot silently measure a broken build.
std::uint64_t calibration_workload(std::uint64_t iterations) {
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        state += i;
    }
    return state;
}

struct Options {
    std::uint64_t iterations = 1'000'000;
    bool json = false;
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") {
            o.json = true;
        } else if (a.rfind("--iterations=", 0) == 0) {
            o.iterations = std::strtoull(a.c_str() + std::strlen("--iterations="), nullptr, 10);
        } else if (a == "--iterations" && i + 1 < argc) {
            o.iterations = std::strtoull(argv[++i], nullptr, 10);
        }
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    const Options opts = parse(argc, argv);
    const hyperion::HostInfo host = hyperion::probe_host();

    const auto start = Clock::now();
    const std::uint64_t checksum = calibration_workload(opts.iterations);
    const auto end = Clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();
    const double ns_per_op = seconds * 1e9 / static_cast<double>(opts.iterations);

    if (opts.json) {
        std::printf("{\n");
        std::printf("  \"schema\": \"hyperion.bench.host.v1\",\n");
        std::printf("  \"build\": \"%s\",\n", hyperion::build_summary().c_str());
        std::printf("  \"host\": {\n");
        std::printf("    \"arch\": \"%s\",\n", hyperion::to_string(host.arch));
        std::printf("    \"kernel\": \"%s\",\n", host.kernel_release.c_str());
        std::printf("    \"logical_cpus\": %u,\n", host.logical_cpus);
        std::printf("    \"page_size\": %zu,\n", host.page_size);
        std::printf("    \"cache_line_size\": %zu\n", host.cache_line_size);
        std::printf("  },\n");
        std::printf("  \"calibration\": {\n");
        std::printf("    \"iterations\": %llu,\n",
                    static_cast<unsigned long long>(opts.iterations));
        std::printf("    \"seconds\": %.9f,\n", seconds);
        std::printf("    \"ns_per_op\": %.4f,\n", ns_per_op);
        std::printf("    \"checksum\": \"0x%016llx\"\n", static_cast<unsigned long long>(checksum));
        std::printf("  },\n");
        // Explicitly unavailable until the translator is implemented.
        std::printf("  \"guest_execution\": {\n");
        std::printf("    \"available\": false,\n");
        std::printf("    \"reason\": \"translator not implemented (Phase 0)\"\n");
        std::printf("  }\n");
        std::printf("}\n");
    } else {
        std::printf("hyperion host benchmark\n");
        std::printf("%s", hyperion::format_host_info(host).c_str());
        std::printf("iterations:      %llu\n", static_cast<unsigned long long>(opts.iterations));
        std::printf("elapsed:         %.6f s\n", seconds);
        std::printf("ns/op:           %.4f\n", ns_per_op);
        std::printf("checksum:        0x%016llx\n", static_cast<unsigned long long>(checksum));
        std::printf("guest execution: unavailable (translator not implemented)\n");
    }
    return 0;
}
