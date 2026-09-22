// Tests for graph metadata computation (ESD.md section 5).
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "lpq/graph/graph_metadata.hpp"
#include "lpq_test/graph_fixtures.hpp"

namespace {

using lpq::compute_metadata;
using lpq::Directedness;
using lpq::GraphBuilder;
using lpq::GraphBuildOptions;
using lpq::InputEdge;
using lpq::WeightKind;

using Builder = GraphBuilder<std::uint32_t>;
using Edge    = InputEdge<std::uint32_t>;

TEST(GraphMetadata, EmptyGraphProducesZeroedStatistics) {
    const auto g = Builder::build(0, {}, GraphBuildOptions{});
    const auto m = compute_metadata(g);
    EXPECT_EQ(m.node_count, 0U);
    EXPECT_EQ(m.arc_count, 0U);
    EXPECT_EQ(m.mean_weight, 0.0);
    EXPECT_EQ(m.mean_out_degree, 0.0);
    EXPECT_EQ(m.density(), 0.0) << "must not divide by a zero node count";
}

TEST(GraphMetadata, NodesWithoutArcsAreCounted) {
    const auto g = Builder::build(4, {}, GraphBuildOptions{});
    const auto m = compute_metadata(g);
    EXPECT_EQ(m.node_count, 4U);
    EXPECT_EQ(m.arc_count, 0U);
    EXPECT_EQ(m.isolated_nodes, 4U);
    EXPECT_EQ(m.max_out_degree, 0U);
}

TEST(GraphMetadata, WeightStatisticsMatchTheFixture) {
    // diamond_graph weights: 1, 4, 1, 2, 3 -> min 1, max 4, mean 2.2
    const auto g = lpq_test::diamond_graph();
    const auto m = compute_metadata(g);

    EXPECT_EQ(m.node_count, 4U);
    EXPECT_EQ(m.arc_count, 5U);
    EXPECT_DOUBLE_EQ(m.min_weight, 1.0);
    EXPECT_DOUBLE_EQ(m.max_weight, 4.0);
    EXPECT_DOUBLE_EQ(m.mean_weight, 11.0 / 5.0);
    // Population variance of {1,4,1,2,3} about mean 2.2 is 1.36.
    EXPECT_NEAR(m.stddev_weight, std::sqrt(1.36), 1e-12);
    EXPECT_TRUE(m.all_weights_positive);
    EXPECT_FALSE(m.all_weights_zero);
}

TEST(GraphMetadata, DegreeStatisticsMatchTheFixture) {
    // diamond_graph out-degrees: 2, 2, 1, 0 -> mean 1.25
    const auto g = lpq_test::diamond_graph();
    const auto m = compute_metadata(g);

    EXPECT_EQ(m.min_out_degree, 0U);
    EXPECT_EQ(m.max_out_degree, 2U);
    EXPECT_DOUBLE_EQ(m.mean_out_degree, 1.25);
    EXPECT_EQ(m.isolated_nodes, 1U) << "node 3 has no outgoing arcs";
}

TEST(GraphMetadata, IdentityFieldsArePropagated) {
    const auto g = lpq_test::diamond_graph();
    const auto m = compute_metadata(g);
    EXPECT_EQ(m.dataset_id, "fixture:diamond");
    EXPECT_EQ(m.directedness, Directedness::kDirected);
    EXPECT_EQ(m.weight_kind, WeightKind::kUInt32);
}

TEST(GraphMetadata, AllZeroWeightsIsDetected) {
    // Signals the hardest tie-breaking case to the rest of the system.
    const auto g = lpq_test::zero_weight_graph<std::uint32_t>(5);
    const auto m = compute_metadata(g);
    EXPECT_TRUE(m.all_weights_zero);
    EXPECT_FALSE(m.all_weights_positive);
    EXPECT_DOUBLE_EQ(m.max_weight, 0.0);
    EXPECT_DOUBLE_EQ(m.stddev_weight, 0.0);
}

TEST(GraphMetadata, MixedZeroAndPositiveWeights) {
    std::vector<Edge> edges{{0, 1, 0}, {1, 2, 5}};
    const auto        g = Builder::build(3, edges, GraphBuildOptions{});
    const auto        m = compute_metadata(g);
    EXPECT_FALSE(m.all_weights_zero);
    EXPECT_FALSE(m.all_weights_positive) << "a zero weight is present";
}

TEST(GraphMetadata, IdenticalWeightsGiveNonNegativeVariance) {
    // Guards the catastrophic-cancellation clamp: the raw second moment can
    // make the variance marginally negative when every weight is equal, and
    // sqrt of a negative would produce NaN.
    std::vector<Edge> edges;
    for (std::uint32_t i = 0; i < 1000; ++i) {
        edges.push_back({i, (i + 1) % 1000, 1000000U});
    }
    const auto g = Builder::build(1000, edges, GraphBuildOptions{});
    const auto m = compute_metadata(g);
    EXPECT_GE(m.stddev_weight, 0.0);
    EXPECT_FALSE(std::isnan(m.stddev_weight));
    EXPECT_NEAR(m.stddev_weight, 0.0, 1e-6);
}

TEST(GraphMetadata, UndirectedEdgeCountIsHalvedButArcCountIsNot) {
    const auto g = lpq_test::star_graph<std::uint32_t>(6);
    const auto m = compute_metadata(g);
    EXPECT_EQ(m.arc_count, 10U);
    EXPECT_EQ(m.edge_count, 5U);
}

TEST(GraphMetadata, DensityIsArcsPerNode) {
    const auto g = lpq_test::diamond_graph();
    const auto m = compute_metadata(g);
    EXPECT_DOUBLE_EQ(m.density(), 5.0 / 4.0);
}

TEST(GraphMetadata, MaxPathCostBoundIsAValidUpperBound) {
    // The bound sizes radix-heap buckets, so it must never be exceeded by a
    // real path cost. For the diamond fixture the longest path 0->1->2->3
    // costs 1 + 1 + 3 = 5, against a bound of 3 * 4 = 12.
    const auto g = lpq_test::diamond_graph();
    const auto m = compute_metadata(g);
    EXPECT_DOUBLE_EQ(m.max_path_cost_bound(), 12.0);
    EXPECT_GE(m.max_path_cost_bound(), 5.0);
}

TEST(GraphMetadata, MaxPathCostBoundIsZeroForTrivialGraphs) {
    const auto g0 = Builder::build(0, {}, GraphBuildOptions{});
    EXPECT_DOUBLE_EQ(compute_metadata(g0).max_path_cost_bound(), 0.0);

    const auto g1 = Builder::build(1, {}, GraphBuildOptions{});
    EXPECT_DOUBLE_EQ(compute_metadata(g1).max_path_cost_bound(), 0.0);
}

TEST(GraphMetadata, CoordinateSystemIsPropagated) {
    const auto g = lpq_test::grid_graph<std::uint32_t>(3, 3);
    const auto m = compute_metadata(g);
    EXPECT_EQ(m.coord_system, lpq::CoordinateSystem::kEuclidean);
}

}  // namespace
