# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# CMake driver script: configure + build + run the test suite for AArch64
# under qemu-user. Invoked as a CTest test by the native build tree.
#
# Required variables:
#   HYPERION_SOURCE_DIR       - repository root
#   HYPERION_CROSS_BINARY_DIR - scratch build tree for the cross build
#   HYPERION_AARCH64_CXX      - AArch64 C++ compiler (e.g. aarch64-linux-gnu-g++)
#   HYPERION_QEMU             - AArch64 user-mode emulator (e.g. qemu-aarch64)

foreach(required HYPERION_SOURCE_DIR HYPERION_CROSS_BINARY_DIR HYPERION_AARCH64_CXX HYPERION_QEMU)
    if(NOT ${required})
        message(FATAL_ERROR "cross tests: '${required}' is not set")
    endif()
endforeach()

find_program(HYPERION_NINJA ninja)
if(NOT HYPERION_NINJA)
    message(FATAL_ERROR "cross tests: 'ninja' not found")
endif()

set(toolchain "${HYPERION_SOURCE_DIR}/cmake/toolchains/aarch64-linux-gnu.cmake")

execute_process(
    COMMAND ${CMAKE_COMMAND}
        -S "${HYPERION_SOURCE_DIR}"
        -B "${HYPERION_CROSS_BINARY_DIR}"
        -G Ninja
        -DCMAKE_TOOLCHAIN_FILE=${toolchain}
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
        -DCMAKE_CXX_COMPILER=${HYPERION_AARCH64_CXX}
        -DHYPERION_BUILD_BENCHMARKS=OFF
    RESULT_VARIABLE cfg_result
    OUTPUT_VARIABLE cfg_output
    ERROR_VARIABLE cfg_error)
if(NOT cfg_result EQUAL 0)
    message(FATAL_ERROR "AArch64 configure failed:\n${cfg_output}\n${cfg_error}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${HYPERION_CROSS_BINARY_DIR}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "AArch64 build failed:\n${build_output}\n${build_error}")
endif()

set(test_binary "${HYPERION_CROSS_BINARY_DIR}/tests/hyperion_unit_tests")
if(NOT EXISTS "${test_binary}")
    message(FATAL_ERROR "cross tests: expected test binary not found at ${test_binary}")
endif()

execute_process(
    COMMAND ${HYPERION_QEMU} "${test_binary}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error)
message(STATUS "AArch64 test output:\n${run_output}")
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "AArch64 tests failed (exit ${run_result}):\n${run_error}")
endif()

message(STATUS "cross-architecture validation pipeline OK")
