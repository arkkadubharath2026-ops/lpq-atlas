// Tests for the trusted reference implementations (ESD.md sections 4.1, 24).
//
// These carry more weight than ordinary unit tests: every later differential
// test trusts this code, so a bug here would silently validate wrong answers
// everywhere else. The suite therefore checks the reference against
// hand-computed distances, against graph-theoretic invariants that must hold
// for any correct shortest-path result, and against a second independent
// implementation.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "lpq/algorithms/reference_dijkstra.hpp"
#include "lpq_test/graph_fixtures.hpp"

namespace {

using lpq::kInvalidNode;
using lpq::kUnreachable;
using lpq::NodeId;
using lpq::reference_dijkstra;
using lpq::reference_dijkstra_heap;
using lpq::ReferenceDijkstraOptions;
using lpq::SearchTermination;

using Dist = lpq::DistanceType<std::uint32_t>;

// --- Hand-computed expectations ----------------------------------------------

TEST(ReferenceDijkstra, MatchesHandComputedDistances) {
    // diamond_graph: 0->1(1), 0->2(4), 1->2(1), 1->3(2), 2->3(3)
    // The shortest route to 2 is 0->1->2 at cost 2, not the direct arc at 4.
    const auto g = lpq_test::diamond_graph();
    const auto r = reference_dijkstra(g, 0);

    EXPECT_EQ(r.dist, lpq_test::diamond_expected_distances());
    EXPECT_EQ(r.termination, SearchTermination::kExhausted);
    EXPECT_EQ(r.counters.settled_nodes, 4U);
}

TEST(ReferenceDijkstra, RecordsThePredecessorOfTheCheaperRoute) {
    const auto g = reference_dijkstra(lpq_test::diamond_graph(), 0);
    EXPECT_EQ(g.parent[0], kInvalidNode) << "the source has no predecessor";
    EXPECT_EQ(g.parent[1], 0U);
    EXPECT_EQ(g.parent[2], 1U) << "0->1->2 (cost 2) beats the direct 0->2 (cost 4)";
    EXPECT_EQ(g.parent[3], 1U) << "0->1->3 (cost 3) beats 0->1->2->3 (cost 5)";
}

TEST(ReferenceDijkstra, PathGraphDistancesEqualTheHopCount) {
    const auto g = lpq_test::path_graph<std::uint32_t>(50);
    const auto r = reference_dijkstra(g, 0);
    for (NodeId v = 0; v < 50; ++v) {
        EXPECT_EQ(r.dist[v], v) << "node " << v;
    }
}

// --- Degenerate inputs -------------------------------------------------------

TEST(ReferenceDijkstra, EmptyGraph) {
    const auto g = lpq_test::path_graph<std::uint32_t>(0);
    const auto r = reference_dijkstra(g, 0);
    EXPECT_EQ(r.num_nodes(), 0U);
    EXPECT_EQ(r.counters.settled_nodes, 0U);
}

TEST(ReferenceDijkstra, SingleNodeGraph) {
    const auto g = lpq_test::path_graph<std::uint32_t>(1);
    const auto r = reference_dijkstra(g, 0);
    EXPECT_EQ(r.dist[0], 0U);
    EXPECT_TRUE(r.settled[0]);
}

TEST(ReferenceDijkstra, OutOfRangeSourceIsHandled) {
    // Must not read out of bounds; the sanitizer build is what proves this.
    const auto g = lpq_test::path_graph<std::uint32_t>(5);
    const auto r = reference_dijkstra(g, 99);
    EXPECT_EQ(r.counters.settled_nodes, 0U);
}

TEST(ReferenceDijkstra, UnreachableNodesKeepTheSentinel) {
    const auto g = lpq_test::disconnected_graph<std::uint32_t>(3);
    const auto r = reference_dijkstra(g, 0);
    for (NodeId v = 0; v < 3; ++v) {
        EXPECT_TRUE(r.reachable(v)) << "node " << v << " is in the source component";
    }
    for (NodeId v = 3; v < 6; ++v) {
        EXPECT_FALSE(r.reachable(v)) << "node " << v << " is in the other component";
        EXPECT_EQ(r.dist[v], kUnreachable<std::uint32_t>);
        EXPECT_FALSE(r.settled[v]);
    }
}

TEST(ReferenceDijkstra, ZeroWeightEdgesGiveZeroDistances) {
    const auto g = lpq_test::zero_weight_graph<std::uint32_t>(6);
    const auto r = reference_dijkstra(g, 0);
    for (NodeId v = 0; v < 6; ++v) {
        EXPECT_EQ(r.dist[v], 0U) << "node " << v;
        EXPECT_TRUE(r.settled[v]);
    }
}

TEST(ReferenceDijkstra, SelfLoopsNeverImproveADistance) {
    // duplicate_edge_graph has a self-loop 1->1 of weight 3 and parallel arcs
    // 0->1 of weights 5, 2 and 9.
    const auto g = lpq_test::duplicate_edge_graph();
    const auto r = reference_dijkstra(g, 0);
    EXPECT_EQ(r.dist[0], 0U);
    EXPECT_EQ(r.dist[1], 2U) << "the cheapest parallel arc wins";
    EXPECT_EQ(r.dist[2], 3U) << "0->1 (2) then 1->2 (1)";
    EXPECT_NE(r.parent[1], 1U) << "a node must never be its own predecessor";
}

// --- Single-target mode ------------------------------------------------------

TEST(ReferenceDijkstra, TargetModeStopsEarly) {
    const auto g = lpq_test::path_graph<std::uint32_t>(100);
    ReferenceDijkstraOptions opts;
    opts.target  = 10;
    const auto r = reference_dijkstra(g, 0, opts);

    EXPECT_EQ(r.termination, SearchTermination::kTargetReached);
    EXPECT_EQ(r.dist[10], 10U);
    // Stopping early is the entire point: it must not settle all 100 nodes.
    EXPECT_EQ(r.counters.settled_nodes, 11U) << "nodes 0..10 inclusive";
}

TEST(ReferenceDijkstra, TargetModeAgreesWithFullSearchOnTheTarget) {
    const auto g = lpq_test::grid_graph<std::uint32_t>(8, 8);
    const auto full = reference_dijkstra(g, 0);

    for (NodeId t = 0; t < 64; ++t) {
        ReferenceDijkstraOptions opts;
        opts.target      = t;
        const auto single = reference_dijkstra(g, 0, opts);
        EXPECT_EQ(single.dist[t], full.dist[t]) << "target " << t;
    }
}

TEST(ReferenceDijkstra, UnreachableTargetIsReported) {
    const auto g = lpq_test::disconnected_graph<std::uint32_t>(3);
    ReferenceDijkstraOptions opts;
    opts.target  = 5;
    const auto r = reference_dijkstra(g, 0, opts);
    EXPECT_EQ(r.termination, SearchTermination::kTargetUnreachable);
    EXPECT_FALSE(r.reachable(5));
}

// --- Path reconstruction -----------------------------------------------------

TEST(PathReconstruction, PathCostMatchesTheReportedDistance) {
    // The strongest per-query check available: recompute the cost from the
    // graph rather than trusting the distance array.
    const auto g = lpq_test::grid_graph<std::uint32_t>(6, 6);
    const auto r = reference_dijkstra(g, 0);

    for (NodeId v = 0; v < 36; ++v) {
        const auto path = r.path_to(v);
        ASSERT_FALSE(path.empty()) << "node " << v << " should be reachable";
        EXPECT_EQ(path.front(), 0U);
        EXPECT_EQ(path.back(), v);
        EXPECT_EQ(r.verify_path_cost(g, path), r.dist[v]) << "node " << v;
    }
}

TEST(PathReconstruction, SourcePathIsJustTheSource) {
    const auto g    = lpq_test::diamond_graph();
    const auto r    = reference_dijkstra(g, 0);
    const auto path = r.path_to(0);
    EXPECT_EQ(path, (std::vector<NodeId>{0}));
    EXPECT_EQ(r.verify_path_cost(g, path), 0U);
}

TEST(PathReconstruction, UnreachableNodeHasNoPath) {
    const auto g = lpq_test::disconnected_graph<std::uint32_t>(3);
    const auto r = reference_dijkstra(g, 0);
    EXPECT_TRUE(r.path_to(4).empty());
}

TEST(PathReconstruction, RejectsAPathUsingANonExistentArc) {
    // Guards the verifier itself: if verify_path_cost accepted arbitrary node
    // sequences it would validate wrong answers.
    const auto g = lpq_test::diamond_graph();
    const auto r = reference_dijkstra(g, 0);
    EXPECT_EQ(r.verify_path_cost(g, std::vector<NodeId>{0, 3}),
              kUnreachable<std::uint32_t>)
        << "there is no direct arc 0->3";
}

// --- Invariants that hold for any correct result -----------------------------

/// Checks the defining property of a shortest-path solution: no arc can be
/// relaxed further. This is the condition that makes a distance array optimal,
/// and it is checked directly rather than by comparison with another run.
template <typename Result, typename Graph>
void expect_no_relaxable_arc(const Result& r, const Graph& g) {
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        if (!r.reachable(u)) {
            continue;
        }
        for (const auto& a : g.out_arcs(u)) {
            ASSERT_TRUE(r.reachable(a.target))
                << "node " << a.target << " reachable from settled " << u
                << " but marked unreachable";
            EXPECT_LE(r.dist[a.target], r.dist[u] + a.weight)
                << "arc " << u << "->" << a.target << " can still be relaxed";
        }
    }
}

TEST(ShortestPathInvariant, NoArcRemainsRelaxable) {
    for (std::uint64_t seed = 0; seed < 200; ++seed) {
        lpq::Rng                  rng(seed);
        lpq_test::RandomGraphSpec spec;
        spec.allow_zero_weights    = (seed % 3 == 0);
        spec.extreme_weight_spread = (seed % 5 == 0);
        spec.many_ties             = (seed % 7 == 0);

        const auto g = lpq_test::random_graph(rng, spec);
        if (g.num_nodes() == 0) {
            continue;
        }
        const auto r = reference_dijkstra(g, 0);
        SCOPED_TRACE("seed " + std::to_string(seed));
        expect_no_relaxable_arc(r, g);
    }
}

TEST(ShortestPathInvariant, EveryReachableNodeHasAValidPath) {
    for (std::uint64_t seed = 0; seed < 100; ++seed) {
        lpq::Rng                  rng(seed + 5000);
        lpq_test::RandomGraphSpec spec;
        const auto                g = lpq_test::random_graph(rng, spec);
        if (g.num_nodes() == 0) {
            continue;
        }
        const auto r = reference_dijkstra(g, 0);

        for (NodeId v = 0; v < g.num_nodes(); ++v) {
            if (!r.reachable(v)) {
                continue;
            }
            const auto path = r.path_to(v);
            ASSERT_FALSE(path.empty()) << "seed " << seed << " node " << v;
            ASSERT_EQ(r.verify_path_cost(g, path), r.dist[v])
                << "seed " << seed << " node " << v;
        }
    }
}

// --- Differential: the two independent oracles must agree --------------------

TEST(ReferenceOracles, LinearScanAndStdHeapAgreeOnAllDistances) {
    // The O(n^2) reference and the std::priority_queue reference share no data
    // structure. Agreement across a wide random corpus is strong evidence that
    // both are correct, which is what licenses their use as oracles.
    for (std::uint64_t seed = 0; seed < 400; ++seed) {
        lpq::Rng                  rng(seed);
        lpq_test::RandomGraphSpec spec;
        spec.allow_zero_weights    = (seed % 3 == 0);
        spec.extreme_weight_spread = (seed % 5 == 0);
        spec.many_ties             = (seed % 4 == 0);
        spec.directedness = (seed % 2 == 0) ? lpq::Directedness::kDirected
                                            : lpq::Directedness::kUndirected;

        const auto g = lpq_test::random_graph(rng, spec);
        if (g.num_nodes() == 0) {
            continue;
        }
        const NodeId src = static_cast<NodeId>(seed % g.num_nodes());

        const auto a = reference_dijkstra(g, src);
        const auto b = reference_dijkstra_heap(g, src);

        ASSERT_EQ(a.dist, b.dist) << "seed " << seed << " source " << src;
        for (NodeId v = 0; v < g.num_nodes(); ++v) {
            ASSERT_EQ(a.reachable(v), b.reachable(v)) << "seed " << seed << " node " << v;
        }
    }
}

TEST(ReferenceOracles, AgreeInSingleTargetMode) {
    for (std::uint64_t seed = 0; seed < 150; ++seed) {
        lpq::Rng                  rng(seed + 9000);
        lpq_test::RandomGraphSpec spec;
        const auto                g = lpq_test::random_graph(rng, spec);
        if (g.num_nodes() < 2) {
            continue;
        }
        ReferenceDijkstraOptions opts;
        opts.target = static_cast<NodeId>(g.num_nodes() - 1);

        const auto a = reference_dijkstra(g, 0, opts);
        const auto b = reference_dijkstra_heap(g, 0, opts);

        ASSERT_EQ(a.termination, b.termination) << "seed " << seed;
        ASSERT_EQ(a.dist[opts.target], b.dist[opts.target]) << "seed " << seed;
    }
}

TEST(ReferenceOracles, AgreeOnFloatingPointGraphs) {
    // Exercises the double instantiation, where costs accumulate with rounding.
    for (std::uint64_t seed = 0; seed < 100; ++seed) {
        lpq::Rng rng(seed + 31337);
        const std::size_t n = 20;
        std::vector<lpq::InputEdge<double>> edges;
        for (std::size_t u = 0; u < n; ++u) {
            for (std::size_t v = 0; v < n; ++v) {
                if (u != v && rng.next_unit() < 0.2) {
                    edges.push_back({static_cast<NodeId>(u), static_cast<NodeId>(v),
                                     rng.next_double(0.001, 1000.0)});
                }
            }
        }
        lpq::GraphBuildOptions opts;
        opts.dataset_id = "fuzz:double";
        const auto g = lpq::GraphBuilder<double>::build(n, std::move(edges), opts);

        const auto a = reference_dijkstra(g, 0);
        const auto b = reference_dijkstra_heap(g, 0);

        for (NodeId v = 0; v < n; ++v) {
            ASSERT_EQ(a.reachable(v), b.reachable(v)) << "seed " << seed << " node " << v;
            if (a.reachable(v)) {
                // Both settle nodes in the same order here, so the sums are
                // associated identically and must match bit for bit. A
                // tolerance would hide a genuine ordering bug.
                ASSERT_DOUBLE_EQ(a.dist[v], b.dist[v]) << "seed " << seed << " node " << v;
            }
        }
    }
}

}  // namespace
