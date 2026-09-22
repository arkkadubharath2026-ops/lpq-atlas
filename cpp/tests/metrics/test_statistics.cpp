// Tests for robust summary statistics (PRD.md section 22).
//
// Every published timing flows through this code, so the properties that matter
// are checked against hand-computed values rather than against another
// implementation: median and MAD are simple enough to verify by hand, and doing
// so removes any shared-assumption risk.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "lpq/metrics/statistics.hpp"

namespace {

using lpq::summarise;
using lpq::summarise_ns;

TEST(Statistics, EmptySampleIsZeroed) {
    const auto s = summarise({});
    EXPECT_EQ(s.count, 0U);
    EXPECT_EQ(s.median, 0.0);
    EXPECT_EQ(s.mad, 0.0);
    EXPECT_EQ(s.relative_mad(), 0.0) << "must not divide by a zero median";
}

TEST(Statistics, SingleSample) {
    const auto s = summarise({42.0});
    EXPECT_EQ(s.count, 1U);
    EXPECT_DOUBLE_EQ(s.median, 42.0);
    EXPECT_DOUBLE_EQ(s.mean, 42.0);
    EXPECT_DOUBLE_EQ(s.mad, 0.0);
    EXPECT_DOUBLE_EQ(s.stddev, 0.0) << "one sample has no dispersion";
    EXPECT_DOUBLE_EQ(s.min, 42.0);
    EXPECT_DOUBLE_EQ(s.max, 42.0);
}

TEST(Statistics, MedianOfOddCount) {
    // {1,2,3,4,5} -> median 3
    const auto s = summarise({3.0, 1.0, 4.0, 5.0, 2.0});
    EXPECT_DOUBLE_EQ(s.median, 3.0);
}

TEST(Statistics, MedianOfEvenCountAveragesTheCentralPair) {
    // {1,2,3,4} -> (2+3)/2 = 2.5
    const auto s = summarise({4.0, 1.0, 3.0, 2.0});
    EXPECT_DOUBLE_EQ(s.median, 2.5);
}

TEST(Statistics, MadMatchesAHandComputation) {
    // Sample {1,2,3,4,5}: median 3; deviations {2,1,0,1,2};
    // sorted {0,1,1,2,2}; median of deviations = 1.
    const auto s = summarise({1.0, 2.0, 3.0, 4.0, 5.0});
    EXPECT_DOUBLE_EQ(s.median, 3.0);
    EXPECT_DOUBLE_EQ(s.mad, 1.0);
    EXPECT_NEAR(s.mad_scaled, 1.4826, 1e-9);
}

TEST(Statistics, MadIsZeroForIdenticalSamples) {
    const auto s = summarise({7.0, 7.0, 7.0, 7.0});
    EXPECT_DOUBLE_EQ(s.median, 7.0);
    EXPECT_DOUBLE_EQ(s.mad, 0.0);
    EXPECT_DOUBLE_EQ(s.relative_mad(), 0.0);
}

// --- The property the choice of median+MAD exists for ------------------------

TEST(Statistics, MedianResistsAnOutlierThatWrecksTheMean) {
    // Ten consistent timings plus one migration artefact -- the exact shape
    // this hardware produces when a thread moves between P and E cores.
    std::vector<double> samples(10, 100.0);
    samples.push_back(10000.0);

    const auto s = summarise(samples);

    EXPECT_DOUBLE_EQ(s.median, 100.0) << "median must be unmoved by one outlier";
    EXPECT_GT(s.mean, 900.0) << "test premise: the mean should be badly distorted";
    // This is the headline justification: reporting the mean here would claim
    // the operation takes ~1000 ns when ten of eleven observations say 100.
    EXPECT_LT(s.median, s.mean / 9.0);
}

TEST(Statistics, MadResistsAnOutlierThatWrecksTheStddev) {
    std::vector<double> samples(10, 100.0);
    samples.push_back(10000.0);

    const auto s = summarise(samples);

    EXPECT_DOUBLE_EQ(s.mad, 0.0) << "ten identical values: robust dispersion is zero";
    EXPECT_GT(s.stddev, 1000.0) << "test premise: stddev should be badly distorted";
}

TEST(Statistics, MeanMedianDivergenceFlagsDisturbedRuns) {
    // The flag exists so a disturbed measurement is visible rather than
    // averaged away. Outliers are never removed (PRD.md section 22).
    std::vector<double> clean(11, 100.0);
    EXPECT_FALSE(summarise(clean).mean_median_diverges());

    std::vector<double> disturbed(10, 100.0);
    disturbed.push_back(10000.0);
    EXPECT_TRUE(summarise(disturbed).mean_median_diverges());
}

TEST(Statistics, RawSamplesAreNotReorderedByTheCaller) {
    // The caller's vector is retained verbatim in the result record, so
    // summarising must not sort it in place.
    std::vector<double> samples{5.0, 1.0, 3.0};
    const auto          original = samples;
    (void)summarise(samples);
    EXPECT_EQ(samples, original);
}

// --- Percentiles --------------------------------------------------------------

TEST(Statistics, PercentilesAreInterpolatedNotNearestRank) {
    // With 11 samples, nearest-rank p95 would simply be the maximum, making the
    // two indistinguishable. Interpolation keeps p95 informative.
    std::vector<double> samples;
    for (int i = 1; i <= 11; ++i) {
        samples.push_back(static_cast<double>(i));
    }
    const auto s = summarise(samples);
    EXPECT_DOUBLE_EQ(s.min, 1.0);
    EXPECT_DOUBLE_EQ(s.max, 11.0);
    EXPECT_DOUBLE_EQ(s.median, 6.0);
    EXPECT_LT(s.p95, s.max) << "p95 must not collapse onto the maximum";
    EXPECT_GT(s.p95, s.p75);
}

TEST(Statistics, QuartilesAndIqr) {
    std::vector<double> samples{1.0, 2.0, 3.0, 4.0, 5.0};
    const auto          s = summarise(samples);
    EXPECT_DOUBLE_EQ(s.p25, 2.0);
    EXPECT_DOUBLE_EQ(s.p75, 4.0);
    EXPECT_DOUBLE_EQ(s.iqr(), 2.0);
}

TEST(Statistics, PercentilesAreMonotonic) {
    std::vector<double> samples;
    for (int i = 0; i < 100; ++i) {
        samples.push_back(static_cast<double>((i * 37) % 100));
    }
    const auto s = summarise(samples);
    EXPECT_LE(s.min, s.p25);
    EXPECT_LE(s.p25, s.median);
    EXPECT_LE(s.median, s.p75);
    EXPECT_LE(s.p75, s.p95);
    EXPECT_LE(s.p95, s.p99);
    EXPECT_LE(s.p99, s.max);
}

// --- Numerical behaviour ------------------------------------------------------

TEST(Statistics, StddevSurvivesLargeValuesWithSmallSpread) {
    // Timings are large numbers with small relative spread -- precisely where
    // the one-pass variance formula loses the answer to cancellation. The
    // two-pass computation must not.
    std::vector<double> samples;
    for (int i = 0; i < 100; ++i) {
        samples.push_back(1.0e9 + static_cast<double>(i));
    }
    const auto s = summarise(samples);
    EXPECT_GT(s.stddev, 0.0) << "spread must not vanish into cancellation";
    EXPECT_FALSE(std::isnan(s.stddev));
    // Population of 0..99 has stddev about 29.01 (sample: about 29.15).
    EXPECT_NEAR(s.stddev, 29.0, 1.0);
}

TEST(Statistics, SampleVarianceUsesNMinusOne) {
    // {2, 4}: mean 3, sample variance = ((1)^2 + (1)^2)/1 = 2, stddev = sqrt(2)
    const auto s = summarise({2.0, 4.0});
    EXPECT_NEAR(s.stddev, std::sqrt(2.0), 1e-12);
}

TEST(Statistics, NanosecondOverloadConverts) {
    const std::vector<std::uint64_t> ns{100, 200, 300};
    const auto                       s = summarise_ns(ns);
    EXPECT_EQ(s.count, 3U);
    EXPECT_DOUBLE_EQ(s.median, 200.0);
}

TEST(Statistics, RelativeMadIsDimensionless) {
    // Scaling every sample must leave relative MAD unchanged, which is what
    // makes it comparable across graphs of different sizes.
    std::vector<double> a{100.0, 110.0, 90.0, 105.0, 95.0};
    std::vector<double> b;
    for (double v : a) {
        b.push_back(v * 1000.0);
    }
    EXPECT_NEAR(summarise(a).relative_mad(), summarise(b).relative_mad(), 1e-12);
}

}  // namespace
