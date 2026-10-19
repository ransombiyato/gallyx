# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Project HYPERION contributors
#
# Benchmark harness (Phase 1 scaffolding).
#
# IMPORTANT: HYPERION has no guest execution engine yet. This harness measures
# only *host* and *harness* characteristics and records them in a machine
# readable report. It deliberately reports "unavailable" rather than inventing
# guest-execution numbers. See BENCHMARKS.md for the full methodology.

add_executable(hyperion_bench host_bench.cpp)
target_link_libraries(hyperion_bench PRIVATE hyperion::core hyperion_warnings)

add_test(NAME bench.smoke COMMAND hyperion_bench --iterations 1000 --json)
set_tests_properties(bench.smoke PROPERTIES LABELS "bench;smoke")
