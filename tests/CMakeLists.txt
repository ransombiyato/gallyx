# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# Test suite. Uses CTest as the single entry point so that native tests and
# cross-architecture tests (run under qemu-user) look identical to CI.

# Shared test harness, linked into every test binary.
add_library(hyperion_test_harness STATIC harness/test_main.cpp)
target_include_directories(hyperion_test_harness PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(hyperion_test_harness PUBLIC hyperion::core hyperion_warnings)

# --- Native unit tests ------------------------------------------------------
add_executable(hyperion_unit_tests
    unit/test_version.cpp
    unit/test_host_probe.cpp
    unit/test_bench_stats.cpp)
target_link_libraries(hyperion_unit_tests PRIVATE hyperion_test_harness)

add_test(NAME unit.native COMMAND hyperion_unit_tests)

# --- Cross-architecture validation pipeline ---------------------------------
#
# On x86-64 developer hosts the AArch64 backend cannot be executed natively.
# This test proves the *validation path* works: it configures a second build
# tree with the AArch64 cross toolchain, builds the same test suite, runs it
# under qemu-user, and requires it to pass. Phase 3+ reuses exactly this
# mechanism to validate generated AArch64 code against the x86-64 oracle.
option(HYPERION_ENABLE_CROSS_TESTS "Run AArch64 tests under qemu-user" OFF)
set(HYPERION_AARCH64_CXX "" CACHE STRING "AArch64 cross compiler")
set(HYPERION_QEMU_AARCH64 "" CACHE STRING "AArch64 user-mode emulator")

if(HYPERION_ENABLE_CROSS_TESTS)
    add_test(NAME cross.aarch64_tests
        COMMAND ${CMAKE_COMMAND}
            -DHYPERION_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DHYPERION_CROSS_BINARY_DIR=${CMAKE_BINARY_DIR}/cross-aarch64
            -DHYPERION_AARCH64_CXX=${HYPERION_AARCH64_CXX}
            -DHYPERION_QEMU=${HYPERION_QEMU_AARCH64}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_cross_tests.cmake)
endif()
