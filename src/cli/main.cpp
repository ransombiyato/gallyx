// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// HYPERION command-line entry point.
//
// Phase 0 deliberately exposes only inspection subcommands. There is no
// guest execution path yet; see ROADMAP.md for the staged plan.
#include <cstdio>
#include <cstring>
#include <string>

#include "hyperion/host_probe.hpp"
#include "hyperion/version.hpp"

namespace {

void print_usage(const char* argv0) {
    std::printf("hyperion - experimental x86-64 to AArch64 dynamic binary translator\n"
                "\n"
                "usage: %s <command> [options]\n"
                "\n"
                "commands:\n"
                "  version     print version and build identity\n"
                "  probe       probe and report host capabilities\n"
                "  help        show this message\n"
                "\n"
                "note: no guest execution is implemented yet (Phase 0 foundation).\n",
                argv0);
}

int cmd_probe() {
    const hyperion::HostInfo info = hyperion::probe_host();
    std::printf("%s", hyperion::format_host_info(info).c_str());
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
    if (command == "probe") {
        return cmd_probe();
    }
    if (command == "help" || command == "--help" || command == "-h") {
        print_usage(argv[0]);
        return 0;
    }

    std::fprintf(stderr, "hyperion: unknown command '%s'\n\n", command.c_str());
    print_usage(argv[0]);
    return 2;
}
