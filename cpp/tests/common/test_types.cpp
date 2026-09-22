// Tests for the core scalar types and their traits.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

#include "lpq/common/types.hpp"

namespace {

using lpq::Directedness;
using lpq::DistanceType;
using lpq::WeightKind;

TEST(Types, SentinelsAreTheMaximumOfTheirType) {
    EXPECT_EQ(lpq::kInvalidNode, std::numeric_limits<lpq::NodeId>::max());
    EXPECT_EQ(lpq::kInvalidEdge, std::numeric_limits<lpq::EdgeId>::max());
}

TEST(Types, MaxNodeCountReservesTheSentinel) {
    // The largest valid node id must be strictly below the sentinel, otherwise
    // "no predecessor" would be indistinguishable from a real node.
    EXPECT_LT(lpq::kMaxNodeCount, static_cast<std::size_t>(lpq::kInvalidNode) + 1U);
}

TEST(DistanceTraits, IntegerWeightsAccumulateIn64Bits) {
    // A path of many uint32 edges can exceed 2^32; the accumulator must not.
    static_assert(std::is_same_v<DistanceType<std::uint32_t>, std::uint64_t>);
    static_assert(std::is_same_v<DistanceType<std::uint64_t>, std::uint64_t>);
    SUCCEED();
}

TEST(DistanceTraits, DoubleWeightsAccumulateInDouble) {
    static_assert(std::is_same_v<DistanceType<double>, double>);
    SUCCEED();
}

TEST(Unreachable, IntegerSentinelIsTheMaximum) {
    EXPECT_EQ(lpq::kUnreachable<std::uint32_t>, std::numeric_limits<std::uint64_t>::max());
}

TEST(Unreachable, DoubleSentinelIsPositiveInfinity) {
    EXPECT_TRUE(std::isinf(lpq::kUnreachable<double>));
    EXPECT_GT(lpq::kUnreachable<double>, 0.0);
    // Infinity must compare greater than any finite cost, so an unreached node
    // never wins a comparison against a reached one.
    EXPECT_GT(lpq::kUnreachable<double>, 1e308);
}

TEST(WeightKind, MapsCompileTimeTypesToRuntimeTags) {
    EXPECT_EQ(lpq::weight_kind_of<std::uint32_t>(), WeightKind::kUInt32);
    EXPECT_EQ(lpq::weight_kind_of<std::uint64_t>(), WeightKind::kUInt64);
    EXPECT_EQ(lpq::weight_kind_of<double>(), WeightKind::kDouble);
}

TEST(StringForms, AreStableForMetadataFiles) {
    // These strings land in dataset metadata and result records, so a change
    // here would silently invalidate previously stored results.
    EXPECT_EQ(std::string(lpq::to_string(Directedness::kDirected)), "directed");
    EXPECT_EQ(std::string(lpq::to_string(Directedness::kUndirected)), "undirected");
    EXPECT_EQ(std::string(lpq::to_string(WeightKind::kUInt32)), "uint32");
    EXPECT_EQ(std::string(lpq::to_string(WeightKind::kUInt64)), "uint64");
    EXPECT_EQ(std::string(lpq::to_string(WeightKind::kDouble)), "double");
}

TEST(WeightTypeConcept, AcceptsOnlyTheSupportedFamilies) {
    static_assert(lpq::WeightType<std::uint32_t>);
    static_assert(lpq::WeightType<std::uint64_t>);
    static_assert(lpq::WeightType<double>);
    // Signed integers are excluded so that non-negativity is a type-level
    // guarantee rather than a runtime check.
    static_assert(!lpq::WeightType<std::int32_t>);
    static_assert(!lpq::WeightType<bool>);
    // float is excluded: accumulating long road-network paths loses too much
    // precision in 24 bits of mantissa.
    static_assert(!lpq::WeightType<float>);
    SUCCEED();
}

}  // namespace
