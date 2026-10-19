// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "harness/test_harness.hpp"
#include "hyperion/version.hpp"

#include <algorithm>
#include <cctype>
#include <string>

using hyperion::build_summary;
using hyperion::version_string;

HYPERION_TEST("version", "version_string_is_semver") {
    const std::string v = version_string();
    HYPERION_CHECK(!v.empty());
    // MAJOR.MINOR.PATCH: exactly two dots, digits only, no whitespace.
    HYPERION_CHECK_EQ(static_cast<int>(std::count(v.begin(), v.end(), '.')), 2);
    HYPERION_CHECK(v.find(' ') == std::string::npos);
    HYPERION_CHECK(std::all_of(v.begin(), v.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.';
    }));
}

HYPERION_TEST("version", "build_summary_mentions_version") {
    const std::string s = build_summary();
    HYPERION_CHECK(s.find(version_string()) != std::string::npos);
    HYPERION_CHECK(s.find("hyperion") != std::string::npos);
}

HYPERION_TEST("version", "build_summary_names_a_compiler") {
    const std::string s = build_summary();
    const bool known = s.find("gcc") != std::string::npos || s.find("clang") != std::string::npos ||
                       s.find("unknown-compiler") != std::string::npos;
    HYPERION_CHECK(known);
}
