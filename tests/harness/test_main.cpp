// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "harness/test_harness.hpp"

#include <cstring>
#include <string>

namespace hyperion::test {
namespace {

// Supports `--filter=substring` and `--list` for CI ergonomics.
struct Options {
    std::string filter;
    bool list_only = false;
};

Options parse_options(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        constexpr const char* kFilter = "--filter=";
        if (arg.rfind(kFilter, 0) == 0) {
            opts.filter = arg.substr(std::strlen(kFilter));
        } else if (arg == "--list") {
            opts.list_only = true;
        }
    }
    return opts;
}

bool matches(const TestCase& tc, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }
    return (tc.suite + '.' + tc.name).find(filter) != std::string::npos;
}

} // namespace

int run_all(const char* program_name, int argc, char** argv) {
    const Options opts = parse_options(argc, argv);

    if (opts.list_only) {
        for (const TestCase& tc : registry()) {
            std::printf("%s.%s\n", tc.suite.c_str(), tc.name.c_str());
        }
        return 0;
    }

    int passed = 0;
    int failed = 0;
    int skipped = 0;
    for (const TestCase& tc : registry()) {
        if (!matches(tc, opts.filter)) {
            ++skipped;
            continue;
        }
        const std::string label = tc.suite + '.' + tc.name;
        try {
            tc.fn();
            std::printf("[ PASS ] %s\n", label.c_str());
            ++passed;
        } catch (const AssertionFailure& e) {
            std::printf("[ FAIL ] %s\n         %s\n", label.c_str(), e.what());
            ++failed;
        } catch (const std::exception& e) {
            std::printf("[ FAIL ] %s\n         unexpected exception: %s\n", label.c_str(),
                        e.what());
            ++failed;
        }
    }

    std::printf("\n%s: %d passed, %d failed, %d skipped (of %zu registered)\n", program_name,
                passed, failed, skipped, registry().size());
    return failed == 0 ? 0 : 1;
}

} // namespace hyperion::test

int main(int argc, char** argv) {
    return hyperion::test::run_all(argv[0], argc, argv);
}
