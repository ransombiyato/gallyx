// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/version.hpp"

#include <sstream>

namespace hyperion {
namespace {

const char* build_type() {
#ifdef NDEBUG
    return "release";
#else
    return "debug";
#endif
}

} // namespace

std::string version_string() {
    std::ostringstream os;
    os << kVersionMajor << '.' << kVersionMinor << '.' << kVersionPatch;
    return os.str();
}

std::string build_summary() {
    std::ostringstream os;
    os << "hyperion " << version_string() << " [" << build_type() << "] ";
#if defined(__clang__)
    os << "clang " << __clang_major__ << '.' << __clang_minor__ << '.' << __clang_patchlevel__;
#elif defined(__GNUC__)
    os << "gcc " << __GNUC__ << '.' << __GNUC_MINOR__ << '.' << __GNUC_PATCHLEVEL__;
#else
    os << "unknown-compiler";
#endif
    os << " c++" << __cplusplus;
    return os.str();
}

} // namespace hyperion
