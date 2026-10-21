// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// HYPERION command-line entry point.
//
// `version` and `probe`/`status` inspect the build and host. `info` inspects
// an x86-64 ELF64 binary. `run` executes a static x86-64 ELF64 executable or
// raw machine-code blob through either Tier 0 (reference interpreter) or
// Tier 1 (AArch64 translator + block cache).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hyperion/address_space.hpp"
#include "hyperion/elf_loader.hpp"
#include "hyperion/exec_mem.hpp"
#include "hyperion/guest_state.hpp"
#include "hyperion/host_probe.hpp"
#include "hyperion/interpreter.hpp"
#include "hyperion/syscall.hpp"
#include "hyperion/translation_cache.hpp"
#include "hyperion/version.hpp"

namespace {

constexpr std::uint64_t kLoadBase = 0x400000ULL;
constexpr std::uint64_t kDefaultMaxSteps = 1'000'000;
constexpr std::size_t kMaxImageBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxAddressSpaceBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kDefaultHeapBase = 0x20000000ULL;
constexpr std::uint64_t kDefaultMmapBase = 0x40000000ULL;

void print_usage(const char* argv0) {
    std::printf("hyperion - experimental x86-64 to AArch64 dynamic binary translator\n"
                "\n"
                "usage: %s <command> [options]\n"
                "\n"
                "commands:\n"
                "  version     print version and build identity\n"
                "  probe       probe and report host capabilities\n"
                "  status      alias for `probe`\n"
                "  info <file> inspect an x86-64 ELF64 executable\n"
                "  run <file>  execute a static x86-64 ELF64 binary or raw code blob\n"
                "  help        show this message\n"
                "\n"
                "run options:\n"
                "  --tier=0|1     execution tier: 0 = reference interpreter,\n"
                "                 1 = AArch64 translator (default 1)\n"
                "  --max-steps=N  guest instruction limit (default %llu)\n"
                "  --dump         print the final register state\n"
                "  -- <args...>   guest argv tail passed on the initial System V stack\n",
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
    while (!std::feof(f) && !std::ferror(f)) {
        const std::size_t n = std::fread(buffer, 1, sizeof(buffer), f);
        if (n == 0) {
            break;
        }
        out.insert(out.end(), buffer, buffer + n);
        if (out.size() > kMaxImageBytes) {
            std::fclose(f);
            std::fprintf(stderr, "hyperion: '%s' exceeds maximum image size (64 MiB)\n", path);
            return false;
        }
    }
    std::fclose(f);
    return true;
}

int cmd_info(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "hyperion: usage: hyperion info <file>\n");
        return 2;
    }
    std::vector<std::uint8_t> bytes;
    if (!read_file(argv[2], bytes) || bytes.empty()) {
        std::fprintf(stderr, "hyperion: '%s' is empty or unreadable\n", argv[2]);
        return 2;
    }
    const hyperion::ElfInfo info = hyperion::inspect_elf(bytes.data(), bytes.size());
    if (!info.ok()) {
        std::fprintf(stderr, "hyperion: '%s': %s\n", argv[2], hyperion::to_string(info.error));
        return 1;
    }
    const std::string formatted = hyperion::format_elf_info(info, argv[2]);
    std::printf("%s", formatted.c_str());
    return 0;
}

int cmd_run(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "hyperion: run needs a file argument\n");
        return 2;
    }
    std::string path;
    int tier = 1;
    std::uint64_t max_steps = kDefaultMaxSteps;
    bool dump = false;
    std::vector<std::string> guest_args;

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--") {
            for (int j = i + 1; j < argc; ++j) {
                guest_args.emplace_back(argv[j]);
            }
            break;
        }
        if (arg.rfind("--tier=", 0) == 0) {
            char* end = nullptr;
            const long v = std::strtol(arg.c_str() + 7, &end, 10);
            if (*end != '\0' || (v != 0 && v != 1)) {
                std::fprintf(stderr, "hyperion: --tier must be 0 or 1\n");
                return 2;
            }
            tier = static_cast<int>(v);
        } else if (arg.rfind("--max-steps=", 0) == 0) {
            char* end = nullptr;
            const unsigned long long v = std::strtoull(arg.c_str() + 12, &end, 10);
            if (*end != '\0') {
                std::fprintf(stderr, "hyperion: invalid --max-steps value\n");
                return 2;
            }
            max_steps = v;
        } else if (arg == "--dump") {
            dump = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "hyperion: unknown run option '%s'\n", arg.c_str());
            return 2;
        } else if (path.empty()) {
            path = arg;
        } else {
            guest_args.push_back(arg);
        }
    }
    if (path.empty()) {
        std::fprintf(stderr, "hyperion: run needs a file argument\n");
        return 2;
    }

    std::vector<std::uint8_t> code;
    if (!read_file(path.c_str(), code) || code.empty()) {
        std::fprintf(stderr, "hyperion: '%s' is empty or unreadable\n", path.c_str());
        return 2;
    }

    std::vector<std::string> full_argv;
    full_argv.reserve(1 + guest_args.size());
    full_argv.push_back(path);
    full_argv.insert(full_argv.end(), guest_args.begin(), guest_args.end());

    hyperion::GuestAddressSpace memory(kDefaultHeapBase, kDefaultMmapBase, kMaxAddressSpaceBytes);
    hyperion::GuestState state;

    if (hyperion::has_elf_magic(code.data(), code.size())) {
        const hyperion::LoadedElf loaded =
            hyperion::load_elf_image(code.data(), code.size(), memory);
        if (!loaded.ok()) {
            std::fprintf(stderr, "hyperion: '%s': %s\n", path.c_str(),
                         hyperion::to_string(loaded.error));
            return 1;
        }
        const std::uint64_t rsp = hyperion::setup_linux_x86_64_stack(memory, full_argv, &loaded);
        if (rsp == 0) {
            std::fprintf(stderr, "hyperion: failed to initialise guest stack\n");
            return 1;
        }
        state.rip = loaded.entry;
        state.reg(hyperion::Gpr::kRsp) = rsp;
    } else {
        const std::size_t map_bytes = (code.size() + hyperion::GuestAddressSpace::kPageMask) &
                                      ~hyperion::GuestAddressSpace::kPageMask;
        if (!memory.map_image(kLoadBase, nullptr, map_bytes,
                              hyperion::MemPerm::kRead | hyperion::MemPerm::kWrite |
                                  hyperion::MemPerm::kExec) ||
            !memory.write(kLoadBase, code.data(), code.size())) {
            std::fprintf(stderr, "hyperion: failed to map '%s' into guest memory\n", path.c_str());
            return 1;
        }
        const std::uint64_t rsp = hyperion::setup_linux_x86_64_stack(memory, full_argv, nullptr);
        if (rsp == 0) {
            std::fprintf(stderr, "hyperion: failed to initialise guest stack\n");
            return 1;
        }
        state.rip = kLoadBase;
        state.reg(hyperion::Gpr::kRsp) = rsp;
    }

    // Tier 1 needs a host that can execute generated AArch64 code. On an x86-64
    // development host it cannot, so fall back cleanly to Tier 0.
    if (tier == 1 && !hyperion::ExecMem::executable_supported()) {
        std::fprintf(stderr, "hyperion: this host cannot execute generated AArch64 code; "
                             "falling back to the reference interpreter (tier 0)\n");
        tier = 0;
    }

    hyperion::MinimalSyscallHandler syscalls(nullptr, &memory);
    hyperion::RunResult result;
    if (tier == 0) {
        result = hyperion::run_program(memory, state, max_steps, &syscalls);
    } else {
        hyperion::TranslationCache cache;
        result = cache.run(memory, state, max_steps, &syscalls);
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
    if (result.status != hyperion::RunStatus::kOk) {
        return 1;
    }
    if (result.exited_via_syscall && result.exit_code != 0) {
        return static_cast<int>(result.exit_code);
    }
    return 0;
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
    if (command == "probe" || command == "status") {
        return cmd_probe();
    }
    if (command == "info") {
        return cmd_info(argc, argv);
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
