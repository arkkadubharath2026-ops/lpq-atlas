// Tests for the deterministic RNG (PRD.md section 18, ESD.md section 4.5).
//
// The properties that matter here are reproducibility and lack of bias. A
// biased query generator would silently skew which source-target pairs the
// benchmark samples, which is a correctness problem for the *experiment* even
// though every individual shortest path would still be right.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

#include "lpq/common/rng.hpp"

namespace {

using lpq::derive_seed;
using lpq::Rng;

TEST(Rng, SameSeedProducesTheSameStream) {
    Rng a(42), b(42);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(a.next_u64(), b.next_u64()) << "diverged at draw " << i;
    }
}

TEST(Rng, DifferentSeedsProduceDifferentStreams) {
    Rng a(42), b(43);
    bool differs = false;
    for (int i = 0; i < 100 && !differs; ++i) {
        differs = (a.next_u64() != b.next_u64());
    }
    EXPECT_TRUE(differs);
}

TEST(Rng, NextBelowStaysInRange) {
    Rng rng(7);
    for (std::uint64_t bound : {1U, 2U, 3U, 10U, 1000U, 65536U}) {
        for (int i = 0; i < 2000; ++i) {
            EXPECT_LT(rng.next_below(bound), bound) << "bound " << bound;
        }
    }
}

TEST(Rng, NextBelowZeroIsDefined) {
    // A zero bound arises naturally when sampling from an empty candidate set.
    // It must return 0 rather than divide by zero.
    Rng rng(7);
    EXPECT_EQ(rng.next_below(0), 0U);
}

TEST(Rng, NextBelowIsApproximatelyUniform) {
    // Chi-squared goodness of fit over 10 buckets, 100k draws.
    // The critical value at 9 degrees of freedom and p = 0.001 is 27.877; a
    // correct generator exceeds it once in a thousand runs, and the seed is
    // fixed so this test is deterministic rather than flaky.
    constexpr int kBuckets = 10;
    constexpr int kDraws   = 100000;
    Rng           rng(12345);
    std::vector<int> counts(kBuckets, 0);
    for (int i = 0; i < kDraws; ++i) {
        counts[rng.next_below(kBuckets)]++;
    }
    const double expected = static_cast<double>(kDraws) / kBuckets;
    double       chi2     = 0.0;
    for (int c : counts) {
        const double d = c - expected;
        chi2 += d * d / expected;
    }
    EXPECT_LT(chi2, 27.877) << "chi-squared " << chi2 << " suggests a biased generator";
}

TEST(Rng, NextInRangeIsInclusiveAtBothEnds) {
    Rng           rng(99);
    std::set<std::uint64_t> seen;
    for (int i = 0; i < 5000; ++i) {
        const auto v = rng.next_in_range(5, 8);
        ASSERT_GE(v, 5U);
        ASSERT_LE(v, 8U);
        seen.insert(v);
    }
    EXPECT_EQ(seen.size(), 4U) << "every value in [5, 8] should appear";
}

TEST(Rng, NextUnitIsInTheHalfOpenUnitInterval) {
    Rng rng(3);
    for (int i = 0; i < 100000; ++i) {
        const double u = rng.next_unit();
        ASSERT_GE(u, 0.0);
        ASSERT_LT(u, 1.0);
    }
}

TEST(Rng, NextNormalHasTheExpectedMoments) {
    Rng    rng(2024);
    double sum = 0.0, sum_sq = 0.0;
    constexpr int kN = 200000;
    for (int i = 0; i < kN; ++i) {
        const double z = rng.next_normal();
        sum += z;
        sum_sq += z * z;
    }
    const double mean = sum / kN;
    const double var  = sum_sq / kN - mean * mean;
    // Standard error of the mean at n = 2e5 is about 0.0022, so 0.02 is ~9 sigma.
    EXPECT_NEAR(mean, 0.0, 0.02);
    EXPECT_NEAR(var, 1.0, 0.02);
}

TEST(Rng, ShuffleIsAPermutation) {
    Rng              rng(11);
    std::vector<int> v(100U);
    for (int i = 0; i < 100; ++i) {
        v[i] = i;
    }
    auto original = v;
    rng.shuffle(v);

    EXPECT_NE(v, original) << "a 100-element shuffle should not be the identity";
    std::sort(v.begin(), v.end());
    EXPECT_EQ(v, original) << "shuffle must preserve the multiset of elements";
}

TEST(Rng, ShuffleHandlesDegenerateSizes) {
    Rng              rng(1);
    std::vector<int> empty;
    rng.shuffle(empty);
    EXPECT_TRUE(empty.empty());

    std::vector<int> single{7};
    rng.shuffle(single);
    EXPECT_EQ(single, (std::vector<int>{7}));
}

TEST(DeriveSeed, IsDeterministic) {
    EXPECT_EQ(derive_seed(42, "queries"), derive_seed(42, "queries"));
}

TEST(DeriveSeed, SeparatesStreamsByLabel) {
    // One manifest seed must drive independent streams, so different labels
    // must not collide.
    EXPECT_NE(derive_seed(42, "queries"), derive_seed(42, "weights"));
    EXPECT_NE(derive_seed(42, "queries"), derive_seed(43, "queries"));
}

TEST(DeriveSeed, DerivedStreamsAreNotCorrelated) {
    // The failure this guards against: derived seeds that differ by a small
    // constant, which in a linear generator can yield visibly similar streams.
    Rng a(derive_seed(42, "queries"));
    Rng b(derive_seed(42, "weights"));
    int matches = 0;
    for (int i = 0; i < 1000; ++i) {
        if (a.next_below(100) == b.next_below(100)) {
            ++matches;
        }
    }
    // Expected matches for independent streams is 10; 50 is far into the tail.
    EXPECT_LT(matches, 50) << "derived streams appear correlated";
}

}  // namespace
