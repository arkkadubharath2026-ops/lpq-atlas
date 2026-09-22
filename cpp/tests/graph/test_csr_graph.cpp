// Tests for the CSR graph's invariants and accessors (ESD.md section 24: CSR
// construction is named as a required unit-test target).
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/graph/csr_graph.hpp"
#include "lpq_test/graph_fixtures.hpp"

namespace {

using lpq::Arc;
using lpq::Coordinate;
using lpq::CoordinateSystem;
using lpq::CsrGraph;
using lpq::Directedness;
using lpq::EdgeId;
using lpq::InvalidGraphError;
using lpq::NodeId;

using GraphU32 = CsrGraph<std::uint32_t>;
using ArcU32   = Arc<std::uint32_t>;

TEST(CsrGraph, DefaultConstructedGraphIsEmpty) {
    GraphU32 g;
    EXPECT_TRUE(g.empty());
    EXPECT_EQ(g.num_nodes(), 0U);
    EXPECT_EQ(g.num_arcs(), 0U);
}

TEST(CsrGraph, ZeroNodeGraphIsLegal) {
    // Not a pathological input to reject: the fuzz corpus generates it, and
    // every algorithm must handle it without special-casing.
    GraphU32 g({}, {}, Directedness::kDirected);
    EXPECT_TRUE(g.empty());
}

TEST(CsrGraph, NodesWithNoArcsAreRepresentable) {
    // Three nodes, no arcs at all.
    GraphU32 g({0, 0, 0, 0}, {}, Directedness::kDirected);
    EXPECT_EQ(g.num_nodes(), 3U);
    EXPECT_EQ(g.num_arcs(), 0U);
    for (NodeId u = 0; u < 3; ++u) {
        EXPECT_EQ(g.out_degree(u), 0U);
        EXPECT_TRUE(g.out_arcs(u).empty());
    }
}

TEST(CsrGraph, AccessorsAgreeWithTheFixture) {
    const auto g = lpq_test::diamond_graph();
    EXPECT_EQ(g.num_nodes(), 4U);
    EXPECT_EQ(g.num_arcs(), 5U);
    EXPECT_EQ(g.num_edges(), 5U) << "directed: edges equal arcs";

    EXPECT_EQ(g.out_degree(0), 2U);  // 0->1, 0->2
    EXPECT_EQ(g.out_degree(1), 2U);  // 1->2, 1->3
    EXPECT_EQ(g.out_degree(2), 1U);  // 2->3
    EXPECT_EQ(g.out_degree(3), 0U);
}

TEST(CsrGraph, OutArcsSpanMatchesTheOffsetRange) {
    const auto g = lpq_test::diamond_graph();
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        const auto span = g.out_arcs(u);
        EXPECT_EQ(span.size(), g.out_degree(u));
        EXPECT_EQ(span.size(), g.arc_end(u) - g.arc_begin(u));
        // The span must alias the shared arc array, not a copy.
        for (std::size_t i = 0; i < span.size(); ++i) {
            EXPECT_EQ(&span[i], &g.arc(g.arc_begin(u) + i));
        }
    }
}

TEST(CsrGraph, UndirectedGraphsHalveTheEdgeCount) {
    const auto g = lpq_test::star_graph<std::uint32_t>(5);
    EXPECT_EQ(g.num_arcs(), 8U) << "4 edges stored as 8 directed arcs";
    EXPECT_EQ(g.num_edges(), 4U);
    EXPECT_EQ(g.directedness(), Directedness::kUndirected);
}

// --- Validation --------------------------------------------------------------

TEST(CsrGraphValidation, RejectsNonZeroFirstOffset) {
    EXPECT_THROW(GraphU32({1, 1}, {}, Directedness::kDirected), InvalidGraphError);
}

TEST(CsrGraphValidation, RejectsOffsetsThatDoNotEndAtTheArcCount) {
    std::vector<ArcU32> arcs{{0, 1}};
    EXPECT_THROW(GraphU32({0, 0}, arcs, Directedness::kDirected), InvalidGraphError);
}

TEST(CsrGraphValidation, RejectsDecreasingOffsets) {
    // A truncated or shuffled dataset file produces exactly this shape, and it
    // would otherwise yield a span with a negative length.
    std::vector<ArcU32> arcs{{0, 1}, {0, 1}};
    EXPECT_THROW(GraphU32({0, 2, 1, 2}, arcs, Directedness::kDirected), InvalidGraphError);
}

TEST(CsrGraphValidation, RejectsArcTargetsOutOfRange) {
    std::vector<ArcU32> arcs{{7, 1}};  // only 2 nodes exist
    EXPECT_THROW(GraphU32({0, 1, 1}, arcs, Directedness::kDirected), InvalidGraphError);
}

TEST(CsrGraphValidation, RejectsArcsWithoutOffsets) {
    std::vector<ArcU32> arcs{{0, 1}};
    EXPECT_THROW(GraphU32({}, arcs, Directedness::kDirected), InvalidGraphError);
}

TEST(CsrGraphValidation, RejectsNegativeDoubleWeights) {
    // Dijkstra's correctness argument depends on non-negativity, so this must
    // fail at load time rather than produce a plausible wrong answer.
    using GraphF64 = CsrGraph<double>;
    std::vector<Arc<double>> arcs{{0, -1.0}};
    EXPECT_THROW(GraphF64({0, 1, 1}, arcs, Directedness::kDirected), InvalidGraphError);
}

TEST(CsrGraphValidation, RejectsNaNWeights) {
    // NaN makes every comparison false, which would silently corrupt heap
    // ordering instead of failing.
    using GraphF64 = CsrGraph<double>;
    std::vector<Arc<double>> arcs{{0, std::numeric_limits<double>::quiet_NaN()}};
    EXPECT_THROW(GraphF64({0, 1, 1}, arcs, Directedness::kDirected), InvalidGraphError);
}

TEST(CsrGraphValidation, RejectsInfiniteWeights) {
    using GraphF64 = CsrGraph<double>;
    std::vector<Arc<double>> arcs{{0, std::numeric_limits<double>::infinity()}};
    EXPECT_THROW(GraphF64({0, 1, 1}, arcs, Directedness::kDirected), InvalidGraphError);
}

TEST(CsrGraphValidation, AcceptsZeroWeights) {
    // Zero weights are legal and are a deliberate tie-breaking stress case.
    std::vector<ArcU32> arcs{{1, 0}};
    EXPECT_NO_THROW(GraphU32({0, 1, 1}, arcs, Directedness::kDirected));
}

TEST(CsrGraphValidation, RejectsMismatchedCoordinateCount) {
    std::vector<Coordinate> coords{{0.0, 0.0}};  // 1 coordinate, 2 nodes
    EXPECT_THROW(GraphU32({0, 0, 0}, {}, Directedness::kDirected, coords,
                          CoordinateSystem::kEuclidean),
                 InvalidGraphError);
}

TEST(CsrGraphValidation, RejectsDeclaredCoordinateSystemWithoutCoordinates) {
    EXPECT_THROW(GraphU32({0, 0, 0}, {}, Directedness::kDirected, {},
                          CoordinateSystem::kEuclidean),
                 InvalidGraphError);
}

// --- Spatial -----------------------------------------------------------------

TEST(CsrGraph, CoordinatesRoundTrip) {
    const auto g = lpq_test::grid_graph<std::uint32_t>(3, 4);
    ASSERT_TRUE(g.has_coordinates());
    EXPECT_EQ(g.coordinate_system(), CoordinateSystem::kEuclidean);
    EXPECT_EQ(g.coordinates().size(), g.num_nodes());
    // Node index r*cols + c sits at (x=c, y=r).
    EXPECT_EQ(g.coordinate(0), (Coordinate{0.0, 0.0}));
    EXPECT_EQ(g.coordinate(5), (Coordinate{1.0, 1.0}));
    EXPECT_EQ(g.coordinate(11), (Coordinate{3.0, 2.0}));
}

TEST(CsrGraph, GraphsWithoutCoordinatesReportSo) {
    const auto g = lpq_test::diamond_graph();
    EXPECT_FALSE(g.has_coordinates());
    EXPECT_EQ(g.coordinate_system(), CoordinateSystem::kNone);
}

TEST(CsrGraph, MemoryFootprintIsReported) {
    const auto g = lpq_test::grid_graph<std::uint32_t>(10, 10);
    // At minimum the offsets and arcs must be accounted for.
    const std::size_t floor = g.offsets().size() * sizeof(EdgeId) + g.num_arcs() * sizeof(ArcU32);
    EXPECT_GE(g.memory_bytes(), floor);
}

TEST(CsrGraph, WeightKindMatchesTheTemplateArgument) {
    EXPECT_EQ(GraphU32::weight_kind(), lpq::WeightKind::kUInt32);
    EXPECT_EQ(CsrGraph<double>::weight_kind(), lpq::WeightKind::kDouble);
}

}  // namespace
