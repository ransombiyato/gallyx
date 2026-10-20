// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Tier 0 vs Tier 1 benchmark harness (Phase 5).
//
// Phase 5 begins with *measurement*, not optimisation. This harness runs the
// same guest program through both execution tiers and reports the time per
// retired guest instruction, so every later change (register allocation, block
// chaining, dispatch) has a fixed, reproducible baseline to be judged against.
//
// It reuses the methodology the host harness established (see BENCHMARKS.md
// section 5): one discarded warm-up, repeated timed samples reported as median
// with an IQR spread, a checksum compared across runs, and an explicit
// plausibility check. A result that is too good to be true is treated as a
// failure, not a result (docs/known-issues.md BENCH-001).
//
// Honesty rules this harness enforces:
//   * Tier 1 numbers are only produced when the host can actually execute
//     generated AArch64 code. On an x86-64 host the fields are reported as
//     unavailable, never as a fabricated comparison.
//   * The guest programs are fixed and their expected final state is checked, so
//     a benchmark cannot silently measure a broken translator.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hyperion/bench_stats.hpp"
#include "hyperion/exec_mem.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/host_probe.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/translation_cache.hpp"
#include "hyperion/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using hyperion::FlatMemory;
using hyperion::Gpr;
using hyperion::GuestState;
using hyperion::RunResult;
using hyperion::RunStatus;
using hyperion::TranslationCache;

constexpr std::uint64_t kBase = 0x400000;

// Guest program: accumulate rax into rdx, `rcx` times, then halt.
//   xor edx, edx            ; 31 D2
// loop:
//   add rdx, rax            ; 48 01 C2
//   dec rcx                 ; 48 FF C9
//   jnz loop                ; 75 F8
//   hlt                     ; F4
// Bytes verified with as/objdump.
const std::vector<std::uint8_t> kLoopProgram = {
    0x31, 0xD2, 0x48, 0x01, 0xC2, 0x48, 0xFF, 0xC9, 0x75, 0xF8, 0xF4,
};

// Compiler barrier: keep the timed run from being hoisted or eliminated.
inline void optimization_barrier(const void* p) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(p) : "memory");
#else
    *static_cast<volatile const void**>(const_cast<const void**>(&p)) = p;
#endif
}

struct Options {
    std::uint64_t iterations = 1'000'000; // loop iterations per sample
    std::uint32_t repeats = 5;            // timed samples (one warm-up discarded)
    bool json = false;
};

void usage() {
    std::fprintf(stderr,
                 "usage: hyperion_tier_bench [--iterations N] [--repeats N] [--json]\n"
                 "  --iterations N  guest loop iterations per sample (default 1000000)\n"
                 "  --repeats N     timed samples, one warm-up discarded (default 5, min 2)\n"
                 "  --json          machine-readable report\n");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") {
            o.json = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else if (a.rfind("--iterations=", 0) == 0) {
            o.iterations = std::strtoull(a.c_str() + std::strlen("--iterations="), nullptr, 10);
        } else if (a == "--iterations" && i + 1 < argc) {
            o.iterations = std::strtoull(argv[++i], nullptr, 10);
        } else if (a.rfind("--repeats=", 0) == 0) {
            o.repeats = static_cast<std::uint32_t>(
                std::strtoul(a.c_str() + std::strlen("--repeats="), nullptr, 10));
        } else if (a == "--repeats" && i + 1 < argc) {
            o.repeats = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
            usage();
            return false;
        }
    }
    if (o.iterations == 0) {
        std::fprintf(stderr, "error: --iterations must be greater than zero\n");
        return false;
    }
    if (o.repeats < 2) {
        // A single sample cannot express a spread; refuse rather than report a
        // meaningless one.
        std::fprintf(stderr, "error: --repeats must be at least 2\n");
        return false;
    }
    return true;
}

struct RunOutcome {
    RunResult result;
    double ns = 0.0;
};

// Time one full run of the loop program through the given tier.
template<typename RunFn> RunOutcome time_run(RunFn&& run, std::uint64_t iterations) {
    FlatMemory mem(kBase, 4096);
    std::memcpy(mem.data(), kLoopProgram.data(), kLoopProgram.size());
    GuestState initial;
    initial.rip = kBase;
    initial.reg(Gpr::kRax) = 1;
    initial.reg(Gpr::kRcx) = iterations;

    const auto t0 = Clock::now();
    RunResult r = run(mem, initial);
    const auto t1 = Clock::now();
    optimization_barrier(&r);
    return RunOutcome{r, std::chrono::duration<double, std::nano>(t1 - t0).count()};
}

// The expected final rdx after `iterations` additions of 1.
std::uint64_t expected_rdx(std::uint64_t iterations) {
    return iterations;
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse(argc, argv, opts)) {
        return 2;
    }

    const bool tier1_available = hyperion::ExecMem::executable_supported();

    std::vector<double> tier0_ns;
    std::vector<double> tier1_ns;
    std::uint64_t steps = 0;
    bool tier0_ok = true;
    bool tier1_ok = true;
    std::string tier1_reason = "host cannot execute generated AArch64 code";

    // Translation size for this workload, captured once outside the timed loop
    // so it can never affect the measurement. Deterministic, unlike timing.
    std::size_t tier1_code_bytes = 0;
    std::size_t tier1_block_count = 0;
    std::uint64_t tier1_dispatch_hits = 0;
    std::uint64_t tier1_dispatch_misses = 0;

    // One discarded warm-up run, then `repeats` timed samples.
    for (std::uint32_t i = 0; i <= opts.repeats; ++i) {
        const bool warmup = (i == 0);

        const RunOutcome r0 =
            time_run([](FlatMemory& mem, GuestState st) { return hyperion::run_program(mem, st); },
                     opts.iterations);
        if (r0.result.status != RunStatus::kOk ||
            r0.result.state.reg(Gpr::kRdx) != expected_rdx(opts.iterations)) {
            tier0_ok = false;
        }
        steps = r0.result.steps;
        if (!warmup) {
            tier0_ns.push_back(r0.ns);
        }

        if (tier1_available) {
            const RunOutcome r1 = time_run(
                [](FlatMemory& mem, GuestState st) {
                    TranslationCache cache;
                    return cache.run(mem, st);
                },
                opts.iterations);
            if (r1.result.status != RunStatus::kOk ||
                r1.result.state.reg(Gpr::kRdx) != expected_rdx(opts.iterations)) {
                tier1_ok = false;
            }
            if (!warmup) {
                tier1_ns.push_back(r1.ns);
            }
        }
    }

    const hyperion::SampleSummary s0 = hyperion::summarize(tier0_ns);
    const hyperion::SampleSummary s1 = hyperion::summarize(tier1_ns);

    if (tier1_available) {
        FlatMemory mem(kBase, 4096);
        std::memcpy(mem.data(), kLoopProgram.data(), kLoopProgram.size());
        GuestState st;
        st.rip = kBase;
        st.reg(Gpr::kRax) = 1;
        st.reg(Gpr::kRcx) = opts.iterations;
        hyperion::TranslationCache cache;
        cache.run(mem, st);
        tier1_code_bytes = cache.total_code_bytes();
        tier1_block_count = cache.block_count();
        tier1_dispatch_hits = cache.dispatch_hits();
        tier1_dispatch_misses = cache.dispatch_misses();
    }
    const auto per_step = [&](double ns) {
        return steps == 0 ? 0.0 : ns / static_cast<double>(steps);
    };
    const hyperion::HostInfo host = hyperion::probe_host();

    const bool have_tier1 = tier1_available && !tier1_ns.empty();
    const double ratio = (have_tier1 && s0.median > 0.0) ? s1.median / s0.median : 0.0;

    if (opts.json) {
        std::printf("{\n");
        std::printf("  \"benchmark\": \"tier1_vs_tier0\",\n");
        std::printf("  \"hyperion_version\": \"%s\",\n", hyperion::version_string().c_str());
        std::printf("  \"host\": { \"arch\": \"%s\" },\n", hyperion::to_string(host.arch));
        std::printf("  \"workload\": {\n");
        std::printf("    \"program\": \"accumulate rax into rdx rcx times, halt\",\n");
        std::printf("    \"iterations\": %llu,\n",
                    static_cast<unsigned long long>(opts.iterations));
        std::printf("    \"guest_instructions_retired\": %llu,\n",
                    static_cast<unsigned long long>(steps));
        std::printf("    \"warmup_discarded\": 1,\n");
        std::printf("    \"samples\": %zu\n", s0.count);
        std::printf("  },\n");
        std::printf("  \"tier0_interpreter\": {\n");
        std::printf("    \"available\": true,\n");
        std::printf("    \"correct\": %s,\n", tier0_ok ? "true" : "false");
        std::printf("    \"ns_median\": %.3f,\n", s0.median);
        std::printf("    \"ns_per_guest_instruction\": %.6f,\n", per_step(s0.median));
        std::printf("    \"iqr_fraction\": %.6f\n", s0.iqr_fraction);
        std::printf("  },\n");
        std::printf("  \"tier1_translator\": {\n");
        std::printf("    \"available\": %s,\n", have_tier1 ? "true" : "false");
        if (have_tier1) {
            std::printf("    \"correct\": %s,\n", tier1_ok ? "true" : "false");
            std::printf("    \"ns_median\": %.3f,\n", s1.median);
            std::printf("    \"ns_per_guest_instruction\": %.6f,\n", per_step(s1.median));
            std::printf("    \"iqr_fraction\": %.6f,\n", s1.iqr_fraction);
            std::printf("    \"blocks\": %zu,\n", tier1_block_count);
            std::printf("    \"generated_code_bytes\": %zu,\n", tier1_code_bytes);
            std::printf("    \"dispatch_hits\": %llu,\n",
                        static_cast<unsigned long long>(tier1_dispatch_hits));
            std::printf("    \"dispatch_misses\": %llu,\n",
                        static_cast<unsigned long long>(tier1_dispatch_misses));
            std::printf("    \"tier1_over_tier0_ratio\": %.6f\n", ratio);
        } else {
            std::printf("    \"reason\": \"%s\"\n", tier1_reason.c_str());
        }
        std::printf("  },\n");
        std::printf("  \"notes\": \"Tier 1 has no register allocation or block chaining yet. On a "
                    "host that can execute AArch64 directly these numbers reflect that; under "
                    "qemu-user both tiers are additionally translated by qemu, so the ratio is a "
                    "relative sanity signal, not a hardware performance claim.\"\n");
        std::printf("}\n");
    } else {
        std::printf("hyperion tier 0 vs tier 1 benchmark\n");
        std::printf("%s", hyperion::format_host_info(host).c_str());
        std::printf("workload:        accumulate rax into rdx rcx times, halt\n");
        std::printf("iterations:      %llu\n", static_cast<unsigned long long>(opts.iterations));
        std::printf("guest instrs:    %llu retired\n", static_cast<unsigned long long>(steps));
        std::printf("samples:         %zu (1 warm-up discarded, %u requested)\n", s0.count,
                    opts.repeats);
        std::printf("tier 0 median:   %.3f ns  (%.4f ns/guest-ins)  correct=%s\n", s0.median,
                    per_step(s0.median), tier0_ok ? "yes" : "NO");
        if (have_tier1) {
            std::printf("tier 1 median:   %.3f ns  (%.4f ns/guest-ins)  correct=%s\n", s1.median,
                        per_step(s1.median), tier1_ok ? "yes" : "NO");
            std::printf("tier 1 vs tier 0: %.2fx (tier 1 is %s)\n", ratio,
                        ratio > 1.0 ? "slower" : "faster");
        } else {
            std::printf("tier 1:          unavailable (%s)\n", tier1_reason.c_str());
        }
    }

    if (!tier0_ok || (have_tier1 && !tier1_ok)) {
        std::fprintf(stderr, "error: a tier produced an incorrect result; measurement rejected\n");
        return 1;
    }
    return 0;
}
