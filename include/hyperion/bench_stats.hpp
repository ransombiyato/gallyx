// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Summary statistics for benchmark samples.
//
// BENCHMARKS.md promises that results report a central tendency and a spread
// rather than a single sample, and that a warm-up run is discarded. This header
// provides that arithmetic so every benchmark and, later, the Phase 8
// optimisation lab report results the same way.
//
// Deliberately dependency-free and deterministic: no RNG, no floating-point
// surprises. Percentiles use the nearest-rank method on the sorted sample.
#ifndef HYPERION_BENCH_STATS_HPP
#define HYPERION_BENCH_STATS_HPP

#include <cstddef>
#include <vector>

namespace hyperion {

// Summary of a set of samples, in the units the caller supplied.
struct SampleSummary {
    std::size_t count = 0;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double median = 0.0;
    double p25 = 0.0; // first quartile
    double p75 = 0.0; // third quartile
    // Population standard deviation. Reported so a reader can tell a stable
    // measurement from a noisy one instead of trusting the median alone.
    double stddev = 0.0;
    // (p75 - p25) / median, as a fraction. Zero when median is zero.
    // This is the "is this measurement trustworthy" signal: a large value means
    // the host was noisy and the result should not be compared against another.
    double iqr_fraction = 0.0;
};

// Summarize samples. An empty input yields a zeroed summary with count == 0.
SampleSummary summarize(std::vector<double> samples);

} // namespace hyperion

#endif // HYPERION_BENCH_STATS_HPP
