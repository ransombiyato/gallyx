// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Tests for benchmark summary statistics.
//
// These matter because BENCHMARKS.md and the Phase 8 optimisation lab will
// decide accept/reject from these numbers. If the statistics are wrong, a
// regression can be reported as an improvement.
#include <vector>

#include "harness/test_harness.hpp"
#include "hyperion/bench_stats.hpp"

using hyperion::summarize;

HYPERION_TEST("bench_stats", "empty_input_is_zeroed") {
    const auto s = summarize({});
    HYPERION_CHECK(s.count == 0);
    HYPERION_CHECK(s.median == 0.0);
    HYPERION_CHECK(s.mean == 0.0);
    HYPERION_CHECK(s.iqr_fraction == 0.0);
}

HYPERION_TEST("bench_stats", "single_sample_is_its_own_min_median_max") {
    const auto s = summarize({5.0});
    HYPERION_CHECK(s.count == 1);
    HYPERION_CHECK(s.min == 5.0);
    HYPERION_CHECK(s.median == 5.0);
    HYPERION_CHECK(s.max == 5.0);
    HYPERION_CHECK(s.mean == 5.0);
    HYPERION_CHECK(s.stddev == 0.0);
}

HYPERION_TEST("bench_stats", "median_of_odd_sample_is_middle_value") {
    // Deliberately unsorted input: summarize must sort internally.
    const auto s = summarize({30.0, 10.0, 20.0});
    HYPERION_CHECK(s.count == 3);
    HYPERION_CHECK(s.median == 20.0);
    HYPERION_CHECK(s.min == 10.0);
    HYPERION_CHECK(s.max == 30.0);
    HYPERION_CHECK(s.mean == 20.0);
}

HYPERION_TEST("bench_stats", "median_of_even_sample_interpolates") {
    const auto s = summarize({10.0, 20.0, 30.0, 40.0});
    HYPERION_CHECK(s.median == 25.0);
}

HYPERION_TEST("bench_stats", "quartiles_bracket_the_median") {
    // 1..9: p25 = 3, median = 5, p75 = 7 by linear interpolation on rank.
    const std::vector<double> v{9, 8, 7, 6, 5, 4, 3, 2, 1};
    const auto s = summarize(v);
    HYPERION_CHECK(s.median == 5.0);
    HYPERION_CHECK(s.p25 == 3.0);
    HYPERION_CHECK(s.p75 == 7.0);
    // (7 - 3) / 5 = 0.8
    HYPERION_CHECK(s.iqr_fraction > 0.7999 && s.iqr_fraction < 0.8001);
}

HYPERION_TEST("bench_stats", "stddev_is_zero_for_identical_samples") {
    const auto s = summarize({4.0, 4.0, 4.0, 4.0});
    HYPERION_CHECK(s.stddev == 0.0);
    HYPERION_CHECK(s.iqr_fraction == 0.0);
}

HYPERION_TEST("bench_stats", "stddev_matches_hand_computed_value") {
    // Population stddev of {2,4,4,4,5,5,7,9} is 2.0.
    const auto s = summarize({2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0});
    HYPERION_CHECK(s.mean == 5.0);
    HYPERION_CHECK(s.stddev > 1.9999 && s.stddev < 2.0001);
}

HYPERION_TEST("bench_stats", "noisy_set_is_distinguishable_from_stable_set") {
    // This is the property the harness relies on to warn about an untrustworthy
    // measurement: a wide spread must produce a visibly larger iqr_fraction.
    const auto stable = summarize({100.0, 100.1, 100.0, 99.9, 100.0});
    const auto noisy = summarize({50.0, 100.0, 150.0, 200.0, 250.0});
    HYPERION_CHECK(stable.iqr_fraction < 0.01);
    HYPERION_CHECK(noisy.iqr_fraction > stable.iqr_fraction);
}
