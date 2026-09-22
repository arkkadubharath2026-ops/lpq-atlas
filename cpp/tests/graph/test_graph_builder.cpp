// Tests for edge-list to CSR construction, including the edge cases ESD.md
// section 24 names for the fuzz corpus: duplicate edges, zero-weight edges,
// disconnected graphs, high-degree nodes, uneven weights, ties, tiny graphs.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/graph/graph_builder.hpp"
#include "lpq_test/graph_fixtures.hpp"

namespace {

using lpq::Directedness;
using lpq::GraphBuilder;
using lpq::GraphBuildOptions;
using lpq::InputEdge;
using lpq::InvalidGraphError;
using lpq::NodeId;
using lpq::ParallelEdgePolicy;
using lpq::SelfLoopPolicy;

using Builder = GraphBuilder<std::uint32_t>;
using Edge    = InputEdge<std::uint32_t>;

/// Collects a node's adjacency as (target, weight) pairs for easy comparison.
template <typename G>
std::vector<std::pair<NodeId, std::uint32_t>> adjacency(const G& g, NodeId u) {
    std::vector<std::pair<NodeId, std::uint32_t>> out;
    for (const auto& a : g.out_arcs(u)) {
        out.emplace_back(a.target, a.weight);
    }
    return out;
}

TEST(GraphBuilder, BuildsAnEmptyGraph) {
    const auto g = Builder::build(0, {}, GraphBuildOptions{});
    EXPECT_EQ(g.num_nodes(), 0U);
    EXPECT_EQ(g.num_arcs(), 0U);
}

TEST(GraphBuilder, BuildsNodesWithoutEdges) {
    const auto g = Builder::build(5, {}, GraphBuildOptions{});
    EXPECT_EQ(g.num_nodes(), 5U);
    EXPECT_EQ(g.num_arcs(), 0U);
}

TEST(GraphBuilder, SingleNodeGraph) {
    const auto g = Builder::build(1, {}, GraphBuildOptions{});
    EXPECT_EQ(g.num_nodes(), 1U);
    EXPECT_EQ(g.out_degree(0), 0U);
}

TEST(GraphBuilder, PlacesArcsUnderTheCorrectSource) {
    std::vector<Edge> edges{{0, 1, 10}, {2, 0, 20}, {0, 2, 30}};
    const auto        g = Builder::build(3, edges, GraphBuildOptions{});

    EXPECT_EQ(adjacency(g, 0),
              (std::vector<std::pair<NodeId, std::uint32_t>>{{1, 10}, {2, 30}}));
    EXPECT_EQ(adjacency(g, 1), (std::vector<std::pair<NodeId, std::uint32_t>>{}));
    EXPECT_EQ(adjacency(g, 2), (std::vector<std::pair<NodeId, std::uint32_t>>{{0, 20}}));
}

TEST(GraphBuilder, OutputIsIndependentOfInputEdgeOrder) {
    // Two graphs built from the same edges in different orders must be
    // byte-identical, so that dataset load order cannot change tie-breaking.
    std::vector<Edge> a{{0, 1, 5}, {0, 2, 3}, {1, 2, 7}, {2, 0, 1}};
    std::vector<Edge> b{{2, 0, 1}, {1, 2, 7}, {0, 2, 3}, {0, 1, 5}};

    const auto ga = Builder::build(3, a, GraphBuildOptions{});
    const auto gb = Builder::build(3, b, GraphBuildOptions{});

    ASSERT_EQ(ga.num_arcs(), gb.num_arcs());
    for (NodeId u = 0; u < 3; ++u) {
        EXPECT_EQ(adjacency(ga, u), adjacency(gb, u)) << "node " << u;
    }
}

TEST(GraphBuilder, AdjacencyIsSortedByTargetThenWeight) {
    std::vector<Edge> edges{{0, 3, 1}, {0, 1, 9}, {0, 1, 2}, {0, 2, 5}};
    const auto        g = Builder::build(4, edges, GraphBuildOptions{});
    EXPECT_EQ(adjacency(g, 0), (std::vector<std::pair<NodeId, std::uint32_t>>{
                                   {1, 2}, {1, 9}, {2, 5}, {3, 1}}));
}

// --- Undirected --------------------------------------------------------------

TEST(GraphBuilder, UndirectedEdgesBecomeArcPairs) {
    std::vector<Edge>  edges{{0, 1, 7}};
    GraphBuildOptions  opts;
    opts.directedness = Directedness::kUndirected;

    const auto g = Builder::build(2, edges, opts);
    EXPECT_EQ(g.num_arcs(), 2U);
    EXPECT_EQ(g.num_edges(), 1U);
    EXPECT_EQ(adjacency(g, 0), (std::vector<std::pair<NodeId, std::uint32_t>>{{1, 7}}));
    EXPECT_EQ(adjacency(g, 1), (std::vector<std::pair<NodeId, std::uint32_t>>{{0, 7}}));
}

TEST(GraphBuilder, UndirectedSelfLoopIsNotDuplicated) {
    // Mirroring a self-loop would double the node's degree and corrupt the
    // degree statistics the feature extractor normalises against.
    std::vector<Edge>  edges{{0, 0, 3}};
    GraphBuildOptions  opts;
    opts.directedness = Directedness::kUndirected;

    const auto g = Builder::build(1, edges, opts);
    EXPECT_EQ(g.num_arcs(), 1U);
    EXPECT_EQ(g.out_degree(0), 1U);
}

// --- Parallel edges ----------------------------------------------------------

TEST(GraphBuilder, KeepsParallelEdgesByDefault) {
    // The default is fidelity to the source dataset: collapsing arcs would
    // change the priority-queue operation counts that are a reported metric.
    const auto g = lpq_test::duplicate_edge_graph();
    EXPECT_EQ(adjacency(g, 0),
              (std::vector<std::pair<NodeId, std::uint32_t>>{{1, 2}, {1, 5}, {1, 9}}));
}

TEST(GraphBuilder, KeepMinimumCollapsesParallelEdgesToTheLightest) {
    std::vector<Edge>  edges{{0, 1, 5}, {0, 1, 2}, {0, 1, 9}, {0, 2, 4}};
    GraphBuildOptions  opts;
    opts.parallel_edges = ParallelEdgePolicy::kKeepMinimum;

    const auto g = Builder::build(3, edges, opts);
    EXPECT_EQ(adjacency(g, 0),
              (std::vector<std::pair<NodeId, std::uint32_t>>{{1, 2}, {2, 4}}));
}

TEST(GraphBuilder, KeepMinimumPreservesOffsetConsistency) {
    // The collapse rebuilds the offsets array; this checks it stays a valid CSR.
    std::vector<Edge> edges{{0, 1, 5}, {0, 1, 2}, {2, 1, 3}, {2, 1, 1}, {2, 0, 8}};
    GraphBuildOptions opts;
    opts.parallel_edges = ParallelEdgePolicy::kKeepMinimum;

    const auto g = Builder::build(3, edges, opts);
    EXPECT_EQ(g.offsets().front(), 0U);
    EXPECT_EQ(g.offsets().back(), g.num_arcs());
    EXPECT_EQ(g.out_degree(0), 1U);
    EXPECT_EQ(g.out_degree(1), 0U);
    EXPECT_EQ(g.out_degree(2), 2U);
    EXPECT_EQ(adjacency(g, 2),
              (std::vector<std::pair<NodeId, std::uint32_t>>{{0, 8}, {1, 1}}));
}

// --- Self-loops --------------------------------------------------------------

TEST(GraphBuilder, KeepsSelfLoopsByDefault) {
    std::vector<Edge> edges{{0, 0, 3}, {0, 1, 1}};
    const auto        g = Builder::build(2, edges, GraphBuildOptions{});
    EXPECT_EQ(g.out_degree(0), 2U);
}

TEST(GraphBuilder, DropsSelfLoopsOnRequest) {
    std::vector<Edge>  edges{{0, 0, 3}, {0, 1, 1}};
    GraphBuildOptions  opts;
    opts.self_loops = SelfLoopPolicy::kDrop;

    const auto g = Builder::build(2, edges, opts);
    EXPECT_EQ(adjacency(g, 0), (std::vector<std::pair<NodeId, std::uint32_t>>{{1, 1}}));
}

// --- Validation --------------------------------------------------------------

TEST(GraphBuilderValidation, RejectsOutOfRangeSource) {
    std::vector<Edge> edges{{5, 0, 1}};
    EXPECT_THROW((void)Builder::build(2, edges, GraphBuildOptions{}), InvalidGraphError);
}

TEST(GraphBuilderValidation, RejectsOutOfRangeTarget) {
    std::vector<Edge> edges{{0, 5, 1}};
    EXPECT_THROW((void)Builder::build(2, edges, GraphBuildOptions{}), InvalidGraphError);
}

TEST(GraphBuilderValidation, RejectsNegativeDoubleWeights) {
    using BuilderF64 = GraphBuilder<double>;
    std::vector<lpq::InputEdge<double>> edges{{0, 1, -0.5}};
    EXPECT_THROW((void)BuilderF64::build(2, edges, GraphBuildOptions{}), InvalidGraphError);
}

TEST(GraphBuilderValidation, RejectsMismatchedCoordinateCount) {
    std::vector<lpq::Coordinate> coords{{0.0, 0.0}};
    EXPECT_THROW((void)Builder::build(3, {}, GraphBuildOptions{}, coords), InvalidGraphError);
}

// --- Edge cases from the fuzz corpus -----------------------------------------

TEST(GraphBuilder, ZeroWeightEdgesAreAccepted) {
    const auto g = lpq_test::zero_weight_graph<std::uint32_t>(4);
    EXPECT_EQ(g.num_arcs(), 3U);
    for (const auto& a : g.arcs()) {
        EXPECT_EQ(a.weight, 0U);
    }
}

TEST(GraphBuilder, DisconnectedComponentsAreRepresentable) {
    const auto g = lpq_test::disconnected_graph<std::uint32_t>(3);
    EXPECT_EQ(g.num_nodes(), 6U);
    // No arc crosses between {0,1,2} and {3,4,5}.
    for (NodeId u = 0; u < 3; ++u) {
        for (const auto& a : g.out_arcs(u)) {
            EXPECT_LT(a.target, 3U);
        }
    }
}

TEST(GraphBuilder, HighDegreeNodesAreHandled) {
    const auto g = lpq_test::star_graph<std::uint32_t>(1000);
    EXPECT_EQ(g.out_degree(0), 999U);
    for (NodeId u = 1; u < 1000; ++u) {
        EXPECT_EQ(g.out_degree(u), 1U);
    }
}

TEST(GraphBuilder, ExtremeWeightSpreadIsPreserved) {
    std::vector<Edge> edges{{0, 1, 1}, {0, 2, 4000000000U}};
    const auto        g = Builder::build(3, edges, GraphBuildOptions{});
    EXPECT_EQ(adjacency(g, 0),
              (std::vector<std::pair<NodeId, std::uint32_t>>{{1, 1}, {2, 4000000000U}}));
}

TEST(GraphBuilder, RandomGraphsSatisfyTheCsrInvariants) {
    // A broad sweep: whatever the generator produces, the result must be a
    // structurally valid CSR graph.
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        lpq::Rng                   rng(seed);
        lpq_test::RandomGraphSpec  spec;
        spec.allow_zero_weights    = (seed % 3 == 0);
        spec.extreme_weight_spread = (seed % 5 == 0);
        spec.many_ties             = (seed % 7 == 0);
        spec.directedness = (seed % 2 == 0) ? Directedness::kDirected : Directedness::kUndirected;

        const auto g = lpq_test::random_graph(rng, spec);

        ASSERT_EQ(g.offsets().size(), g.num_nodes() + 1U) << "seed " << seed;
        ASSERT_EQ(g.offsets().front(), 0U) << "seed " << seed;
        ASSERT_EQ(g.offsets().back(), g.num_arcs()) << "seed " << seed;

        std::size_t degree_sum = 0;
        for (NodeId u = 0; u < g.num_nodes(); ++u) {
            ASSERT_LE(g.arc_begin(u), g.arc_end(u)) << "seed " << seed << " node " << u;
            degree_sum += g.out_degree(u);
            for (const auto& a : g.out_arcs(u)) {
                ASSERT_LT(a.target, g.num_nodes()) << "seed " << seed;
            }
            // Canonical order within the adjacency.
            const auto span = g.out_arcs(u);
            ASSERT_TRUE(std::is_sorted(span.begin(), span.end(), [](const auto& l, const auto& r) {
                return l.target != r.target ? l.target < r.target : l.weight < r.weight;
            })) << "seed " << seed << " node " << u;
        }
        ASSERT_EQ(degree_sum, g.num_arcs()) << "seed " << seed;
    }
}

}  // namespace
