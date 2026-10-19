// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
#include "hyperion/bench_stats.hpp"

#include <algorithm>
#include <cmath>

namespace hyperion {
namespace {

// Nearest-rank percentile on a sorted, non-empty vector.
// p is a fraction in [0, 1].
double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    if (p <= 0.0) {
        return sorted.front();
    }
    if (p >= 1.0) {
        return sorted.back();
    }
    const double rank = p * static_cast<double>(sorted.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(rank);
    const std::size_t hi = lo + 1 < sorted.size() ? lo + 1 : lo;
    const double frac = rank - static_cast<double>(lo);
    return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
}

} // namespace

SampleSummary summarize(std::vector<double> samples) {
    SampleSummary s;
    if (samples.empty()) {
        return s;
    }

    std::sort(samples.begin(), samples.end());

    s.count = samples.size();
    s.min = samples.front();
    s.max = samples.back();

    double total = 0.0;
    for (const double v : samples) {
        total += v;
    }
    s.mean = total / static_cast<double>(samples.size());

    s.median = percentile(samples, 0.5);
    s.p25 = percentile(samples, 0.25);
    s.p75 = percentile(samples, 0.75);

    double sq = 0.0;
    for (const double v : samples) {
        const double d = v - s.mean;
        sq += d * d;
    }
    s.stddev = std::sqrt(sq / static_cast<double>(samples.size()));

    if (s.median != 0.0) {
        s.iqr_fraction = (s.p75 - s.p25) / s.median;
    }
    return s;
}

} // namespace hyperion
