// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Build and version identification for the HYPERION translator.
#ifndef HYPERION_VERSION_HPP
#define HYPERION_VERSION_HPP

#include <string>

namespace hyperion {

inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;
inline constexpr int kVersionPatch = 0;

// Semantic version string, e.g. "0.1.0".
std::string version_string();

// Human-readable one-line build identity including compiler and build type.
// Recorded in benchmark output so measurements are attributable to a build.
std::string build_summary();

} // namespace hyperion

#endif // HYPERION_VERSION_HPP
