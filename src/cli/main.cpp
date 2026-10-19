// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// HYPERION command-line entry point.
//
// `version` and `probe` inspect the host. `run` executes a raw x86-64 code
// blob through one of the two execution tiers: the Tier 0 reference
// interpreter, or the Tier 1 AArch64 translator. The two must agree; `run
// --tier=0` and `--tier=1` are how that agreement is checked by hand.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hyperion/exec_mem.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/host_probe.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/translation_cache.hpp"
#include "hyperion/version.hpp"

namespace {

constexpr std::uint64_t kLoadBase = 0x400000;
// The blob is loaded into a page-sized region so that a translation window
// (and, later, reads past the end of a short blob) stays inside the mapped
// region instead of faulting on the last instruction.
constexpr std::size_t kMemSize = 4096;
constexpr std::uint64_t kDefaultMaxSteps = 1'000'000;

void print_usage(const char* argv0) {
    std::printf("hyperion - experimental x86-64 to AArch64 dynamic binary translator\n"
                "\n"
                "usage: %s <command> [options]\n"
                "\n"
                "commands:\n"
                "  version     print version and build identity\n"
                "  probe       probe and report host capabilities\n"
                "  run <file>  execute a raw x86-64 code blob\n"
                "  help        show this message\n"
                "\n"
                "run options:\n"
                "  --tier=0|1     execution tier: 0 = reference interpreter,\n"
                "                 1 = AArch64 translator (default 1)\n"
                "  --max-steps=N  guest instruction limit (default %llu)\n"
                "  --dump         print the final register state\n"
                "\n"
                "note: the guest subset is small (see COMPATIBILITY.md); the blob is\n"
                "      loaded at 0x400000 with rip at its first byte.\n",
                argv0, static_cast<unsigned long long>(kDefaultMaxSteps));
}

int cmd_probe() {
    const hyperion::HostInfo info = hyperion::probe_host();
    std::printf("%s", hyperion::format_host_info(info).c_str());
    return 0;
}

bool read_file(const char* path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        std::fprintf(stderr, "hyperion: cannot open '%s'\n", path);
        return false;
    }
    std::uint8_t buffer[4096];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) {
        out.insert(out.end(), buffer, buffer + n);
    }
    std::fclose(f);
    return true;
}

int cmd_run(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "hyperion: run needs a file argument\n");
        return 2;
    }
    const char* path = argv[2];
    int tier = 1;
    std::uint64_t max_steps = kDefaultMaxSteps;
    bool dump = false;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--tier=", 0) == 0) {
            tier = std::atoi(arg.c_str() + 7);
        } else if (arg.rfind("--max-steps=", 0) == 0) {
            max_steps = std::strtoull(arg.c_str() + 12, nullptr, 10);
        } else if (arg == "--dump") {
            dump = true;
        } else {
            std::fprintf(stderr, "hyperion: unknown run option '%s'\n", arg.c_str());
            return 2;
        }
    }
    if (tier != 0 && tier != 1) {
        std::fprintf(stderr, "hyperion: --tier must be 0 or 1\n");
        return 2;
    }

    std::vector<std::uint8_t> code;
    if (!read_file(path, code) || code.empty()) {
        std::fprintf(stderr, "hyperion: '%s' is empty or unreadable\n", path);
        return 2;
    }

    if (code.size() > kMemSize) {
        std::fprintf(stderr, "hyperion: '%s' is larger than the %zu-byte load window\n", path,
                     kMemSize);
        return 2;
    }
    hyperion::FlatMemory memory(kLoadBase, kMemSize);
    std::memcpy(memory.data(), code.data(), code.size());

    hyperion::GuestState state;
    state.rip = kLoadBase;

    // Tier 1 needs a host that can execute the code it generates. On an x86-64
    // development host it cannot, so fall back to the interpreter and say so
    // rather than reporting a mysterious failure.
    if (tier == 1 && !hyperion::ExecMem::executable_supported()) {
        std::fprintf(stderr, "hyperion: this host cannot execute generated AArch64 code; "
                             "falling back to the reference interpreter (tier 0)\n");
        tier = 0;
    }

    hyperion::RunResult result;
    if (tier == 0) {
        result = hyperion::run_program(memory, state, max_steps);
    } else {
        hyperion::TranslationCache cache;
        result = cache.run(memory, state, max_steps);
        std::printf("tier 1: %zu basic block(s) translated\n", cache.block_count());
    }

    std::printf("status: %s\n", hyperion::to_string(result.status));
    std::printf("steps:  %llu\n", static_cast<unsigned long long>(result.steps));
    if (!result.message.empty()) {
        std::printf("detail: %s\n", result.message.c_str());
    }
    if (dump) {
        std::printf("%s", hyperion::to_string(result.state).c_str());
    }
    return result.status == hyperion::RunStatus::kOk ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    const std::string command = argv[1];
    if (command == "version" || command == "--version" || command == "-V") {
        std::printf("%s\n", hyperion::build_summary().c_str());
        return 0;
    }
    if (command == "probe") {
        return cmd_probe();
    }
    if (command == "run") {
        return cmd_run(argc, argv);
    }
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage(argv[0]);
        return 0;
    }

    std::fprintf(stderr, "hyperion: unknown command '%s'\n\n", command.c_str());
    print_usage(argv[0]);
    return 2;
}
