# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# CTest driver: assert that the benchmark harness *rejects* a malformed request.
#
# A single timed sample cannot express a spread, and the warm-up discard would
# leave nothing to report. The harness must fail loudly rather than emit a
# meaningless one-sample "result" that a later phase might compare against.
#
# CTest's WILL_FAIL and PASS_REGULAR_EXPRESSION invert each other, so this needs
# an explicit script to assert both the exit status and the message.
#
# Required variables:
#   HYPERION_BENCH_BIN - path to the hyperion_bench executable

if(NOT HYPERION_BENCH_BIN)
    message(FATAL_ERROR "bench_args: HYPERION_BENCH_BIN is not set")
endif()

execute_process(
    COMMAND "${HYPERION_BENCH_BIN}" --repeats 1
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)

set(combined "${out}${err}")

if(exit_code EQUAL 0)
    message(FATAL_ERROR "harness accepted --repeats 1 and exited 0; it must reject it")
endif()

string(FIND "${combined}" "--repeats must be at least 2" found)
if(found EQUAL -1)
    message(FATAL_ERROR "harness rejected --repeats 1 but without the expected message.\n"
                        "exit=${exit_code}\noutput:\n${combined}")
endif()

message(STATUS "harness correctly rejected --repeats 1 (exit=${exit_code})")
