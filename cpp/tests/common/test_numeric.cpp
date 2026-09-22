// Tests for the numerical comparison policy (ESD.md section 25).
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "lpq/common/numeric.hpp"
#include "lpq/common/types.hpp"

namespace {

using lpq::approx_equal;
using lpq::approximation_ratio;
using lpq::costs_equal;
using lpq::saturating_add;
using lpq::within_epsilon;

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

TEST(ApproxEqual, ExactValuesCompareEqual) {
    EXPECT_TRUE(approx_equal(1.0, 1.0));
    EXPECT_TRUE(approx_equal(0.0, 0.0));
    EXPECT_TRUE(approx_equal(-0.0, 0.0));
}

TEST(ApproxEqual, AbsorbsReassociationError) {
    // The same three values summed in two different orders. This is exactly the
    // discrepancy the tolerance exists to absorb: two correct shortest-path
    // implementations may accumulate a path in different orders.
    const double a = 0.1 + (0.2 + 0.3);
    const double b = (0.1 + 0.2) + 0.3;
    EXPECT_NE(a, b) << "test premise: these must differ in the last bits";
    EXPECT_TRUE(approx_equal(a, b));
}

TEST(ApproxEqual, RejectsDifferencesLargerThanTolerance) {
    EXPECT_FALSE(approx_equal(1.0, 1.0 + 1e-6));
    EXPECT_FALSE(approx_equal(1000.0, 1001.0));
}

TEST(ApproxEqual, ToleranceIsRelativeNotAbsolute) {
    // A 1e-7 absolute gap is significant at magnitude 1 but not at magnitude 1e9.
    EXPECT_FALSE(approx_equal(1.0, 1.0 + 1e-7));
    EXPECT_TRUE(approx_equal(1e9, 1e9 + 1e-7));
}

TEST(ApproxEqual, NearZeroUsesTheAbsoluteFloor) {
    // A relative test alone can never match here; the absolute floor must apply.
    EXPECT_TRUE(approx_equal(0.0, 1e-13));
    EXPECT_FALSE(approx_equal(0.0, 1e-6));
}

TEST(ApproxEqual, NaNNeverValidates) {
    EXPECT_FALSE(approx_equal(kNaN, kNaN));
    EXPECT_FALSE(approx_equal(kNaN, 1.0));
    EXPECT_FALSE(approx_equal(1.0, kNaN));
}

TEST(ApproxEqual, InfinitiesMatchOnlyEachOther) {
    EXPECT_TRUE(approx_equal(kInf, kInf)) << "two unreachable nodes must agree";
    EXPECT_FALSE(approx_equal(kInf, 1e308));
    EXPECT_FALSE(approx_equal(kInf, -kInf));
}

TEST(CostsEqual, IntegerWeightsCompareExactly) {
    // No tolerance for integers: 5 and 6 are different costs, full stop.
    EXPECT_TRUE(costs_equal<std::uint32_t>(5, 5));
    EXPECT_FALSE(costs_equal<std::uint32_t>(5, 6));
}

TEST(CostsEqual, DoubleWeightsUseTheTolerance) {
    EXPECT_TRUE(costs_equal<double>(1.0, 1.0 + 1e-12));
    EXPECT_FALSE(costs_equal<double>(1.0, 1.1));
}

TEST(WithinEpsilon, AcceptsCostsInsideTheBudget) {
    // 105 <= 1.10 * 100.
    EXPECT_TRUE(within_epsilon<std::uint32_t>(105, 100, 0.10));
    EXPECT_TRUE(within_epsilon<std::uint32_t>(110, 100, 0.10)) << "the bound is inclusive";
    EXPECT_FALSE(within_epsilon<std::uint32_t>(111, 100, 0.10));
}

TEST(WithinEpsilon, ZeroEpsilonDemandsOptimality) {
    EXPECT_TRUE(within_epsilon<std::uint32_t>(100, 100, 0.0));
    EXPECT_FALSE(within_epsilon<std::uint32_t>(101, 100, 0.0));
}

TEST(WithinEpsilon, ZeroOptimumAdmitsNoSlack) {
    // A multiplicative budget around zero is still zero, at any epsilon.
    EXPECT_TRUE(within_epsilon<std::uint32_t>(0, 0, 0.5));
    EXPECT_FALSE(within_epsilon<std::uint32_t>(1, 0, 0.5));
}

TEST(ApproximationRatio, ComputesReturnedOverOptimal) {
    EXPECT_DOUBLE_EQ(approximation_ratio<std::uint32_t>(110, 100), 1.1);
    EXPECT_DOUBLE_EQ(approximation_ratio<std::uint32_t>(100, 100), 1.0);
}

TEST(ApproximationRatio, ZeroOptimumDoesNotDivideByZero) {
    EXPECT_DOUBLE_EQ(approximation_ratio<std::uint32_t>(0, 0), 1.0);
    // A non-zero cost against a zero optimum is an infinite ratio, not a crash
    // and not a silently swallowed failure.
    EXPECT_TRUE(std::isinf(approximation_ratio<std::uint32_t>(5, 0)));
}

TEST(SaturatingAdd, DoesNotWrapAtTheIntegerCeiling) {
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    // The bug this prevents: relaxing through an unreached node wrapping around
    // to a tiny distance and inventing a shortest path that does not exist.
    EXPECT_EQ(saturating_add<std::uint32_t>(kMax, 10U), kMax);
    EXPECT_EQ(saturating_add<std::uint32_t>(kMax - 5U, 10U), kMax);
    EXPECT_EQ(saturating_add<std::uint32_t>(100U, 10U), 110U);
}

TEST(SaturatingAdd, DoublesSaturateViaInfinity) {
    EXPECT_EQ(saturating_add<double>(kInf, 10.0), kInf);
    EXPECT_DOUBLE_EQ(saturating_add<double>(1.5, 2.25), 3.75);
}

TEST(Unreachable, SentinelsAreRecognised) {
    EXPECT_TRUE(lpq::is_unreachable<std::uint32_t>(lpq::kUnreachable<std::uint32_t>));
    EXPECT_FALSE(lpq::is_unreachable<std::uint32_t>(0));
    EXPECT_TRUE(lpq::is_unreachable<double>(lpq::kUnreachable<double>));
    EXPECT_FALSE(lpq::is_unreachable<double>(1e300));
}

}  // namespace
