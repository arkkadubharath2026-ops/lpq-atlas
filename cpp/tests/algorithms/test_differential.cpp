// =============================================================================
// Differential correctness testing (ESD.md section 24, Milestone 5).
//
// Runs the same query through every implementation and compares outputs
// automatically: the O(n^2) reference, the std::priority_queue reference, and
// Dijkstra over each of the five queues.
//
// Every randomised case prints its seed on failure, and any seed that ever
// fails is promoted permanently into regression_seeds.hpp. The corpus in that
// file is replayed first, before the moving sweeps, so a previously-found bug
// is caught even if the sweep ranges are later retuned.
// =============================================================================
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "lpq/algorithms/dijkstra.hpp"
#include "lpq/algorithms/reference_dijkstra.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/pq/binary_heap.hpp"
#include "lpq/pq/fibonacci_heap.hpp"
#include "lpq/pq/pairing_heap.hpp"
#include "lpq/pq/radix_heap.hpp"
#include "lpq_test/differential.hpp"
#include "lpq_test/graph_fixtures.hpp"
#include "lpq_test/regression_seeds.hpp"

namespace {

using lpq::dijkstra;
using lpq::DijkstraOptions;
using lpq::Directedness;
using lpq::kInvalidNode;
using lpq::NodeId;
using lpq::reference_dijkstra;
using lpq::reference_dijkstra_heap;
using lpq::Rng;

using W    = std::uint32_t;
using Dist = lpq::DistanceType<W>;
using Graph = lpq::CsrGraph<W>;

// -----------------------------------------------------------------------------
// Running one query through every implementation
// -----------------------------------------------------------------------------

/// Runs `g` from `source` through every queue and checks each against the
/// O(n^2) reference. `context` identifies the case in any failure message.
void run_all_implementations(const Graph& g, NodeId source, const std::string& context,
                             NodeId target = kInvalidNode) {
    lpq::ReferenceDijkstraOptions ref_opts;
    ref_opts.target = target;
    const auto expected = reference_dijkstra(g, source, ref_opts);

    DijkstraOptions opts;
    opts.target       = target;
    opts.measure_time = false;  // hundreds of thousands of tiny queries

    {
        const auto r = reference_dijkstra_heap(g, source, ref_opts);
        lpq_test::expect_results_agree(g, expected, r, context + " [std::priority_queue]");
    }
    {
        const auto r = dijkstra<W, lpq::BinaryHeap<Dist>>(g, source, opts);
        lpq_test::expect_results_agree(g, expected, r, context + " [binary_heap]");
    }
    {
        const auto r = dijkstra<W, lpq::FourAryHeap<Dist>>(g, source, opts);
        lpq_test::expect_results_agree(g, expected, r, context + " [four_ary_heap]");
    }
    {
        const auto r = dijkstra<W, lpq::PairingHeap<Dist>>(g, source, opts);
        lpq_test::expect_results_agree(g, expected, r, context + " [pairing_heap]");
    }
    {
        const auto r = dijkstra<W, lpq::FibonacciHeap<Dist>>(g, source, opts);
        lpq_test::expect_results_agree(g, expected, r, context + " [fibonacci_heap]");
    }
    if constexpr (lpq::radix_heap_supports<W>()) {
        // Dijkstra with non-negative weights produces monotone keys, which is
        // the radix heap's precondition.
        const auto r = dijkstra<W, lpq::RadixHeap<Dist>>(g, source, opts);
        lpq_test::expect_results_agree(g, expected, r, context + " [radix_heap]");
    }
}

// -----------------------------------------------------------------------------
// Permanent regression corpus -- replayed before any moving sweep
// -----------------------------------------------------------------------------

TEST(DifferentialRegression, PreviouslyFailingSeedsStillPass) {
    const auto& seeds = lpq_test::differential_regression_seeds();
    if (seeds.empty()) {
        GTEST_SKIP() << "no regression seeds recorded yet; the corpus grows as "
                        "randomised sweeps find failures";
    }
    for (const auto& rs : seeds) {
        Rng                       rng(rs.seed);
        lpq_test::RandomGraphSpec spec;
        const auto                g = lpq_test::random_graph(rng, spec);
        if (g.num_nodes() == 0) {
            continue;
        }
        const std::string ctx = "REGRESSION seed=" + std::to_string(rs.seed) + " (" +
                                std::string(rs.date) + ")";
        // Replay in both modes. Seed 1002 was a target-mode defect, so a
        // full-SSSP-only replay would not have caught it.
        run_all_implementations(g, 0, ctx + " [sssp]");
        if (g.num_nodes() >= 2) {
            run_all_implementations(g, 0, ctx + " [target]",
                                    static_cast<NodeId>(g.num_nodes() - 1));
        }
    }
}

// -----------------------------------------------------------------------------
// Named fixtures
// -----------------------------------------------------------------------------

TEST(Differential, DiamondFixture) {
    run_all_implementations(lpq_test::diamond_graph(), 0, "diamond");
}

TEST(Differential, PathGraph) {
    run_all_implementations(lpq_test::path_graph<W>(200), 0, "path/200");
}

TEST(Differential, StarGraph) {
    run_all_implementations(lpq_test::star_graph<W>(500), 0, "star/500");
}

TEST(Differential, GridGraph) {
    run_all_implementations(lpq_test::grid_graph<W>(20, 20), 0, "grid/20x20");
}

TEST(Differential, DisconnectedGraph) {
    run_all_implementations(lpq_test::disconnected_graph<W>(25), 0, "disconnected");
}

TEST(Differential, ZeroWeightGraph) {
    // Every reachable node ties at distance zero: maximal tie pressure, where
    // queues differ most in which node they return.
    run_all_implementations(lpq_test::zero_weight_graph<W>(50), 0, "zero_weights");
}

TEST(Differential, DuplicateEdgeGraph) {
    run_all_implementations(lpq_test::duplicate_edge_graph(), 0, "duplicate_edges");
}

TEST(Differential, SingleNodeGraph) {
    run_all_implementations(lpq_test::path_graph<W>(1), 0, "single_node");
}

TEST(Differential, EmptyGraph) {
    run_all_implementations(lpq_test::path_graph<W>(0), 0, "empty");
}

// -----------------------------------------------------------------------------
// Randomised sweeps -- looking for NEW failures
// -----------------------------------------------------------------------------

TEST(Differential, RandomGraphSweep) {
    for (std::uint64_t seed = 0; seed < 600; ++seed) {
        Rng                       rng(seed);
        lpq_test::RandomGraphSpec spec;
        spec.allow_zero_weights    = (seed % 3 == 0);
        spec.extreme_weight_spread = (seed % 5 == 0);
        spec.many_ties             = (seed % 4 == 0);
        spec.directedness = (seed % 2 == 0) ? Directedness::kDirected : Directedness::kUndirected;

        const auto g = lpq_test::random_graph(rng, spec);
        if (g.num_nodes() == 0) {
            continue;
        }
        const NodeId source = static_cast<NodeId>(seed % g.num_nodes());
        run_all_implementations(g, source, "seed=" + std::to_string(seed) +
                                               " source=" + std::to_string(source));
    }
}

TEST(Differential, RandomGraphSweepSingleTarget) {
    for (std::uint64_t seed = 1000; seed < 1300; ++seed) {
        Rng                       rng(seed);
        lpq_test::RandomGraphSpec spec;
        spec.many_ties = (seed % 3 == 0);

        const auto g = lpq_test::random_graph(rng, spec);
        if (g.num_nodes() < 2) {
            continue;
        }
        const NodeId source = 0;
        const NodeId target = static_cast<NodeId>(g.num_nodes() - 1);
        run_all_implementations(g, source, "seed=" + std::to_string(seed) + " target-mode",
                                target);
    }
}

TEST(Differential, DenseGraphSweep) {
    // High edge probability: many parallel improvements per node, which drives
    // decrease-key traffic and is where heap bugs surface.
    for (std::uint64_t seed = 2000; seed < 2100; ++seed) {
        Rng                       rng(seed);
        lpq_test::RandomGraphSpec spec;
        spec.min_nodes        = 20;
        spec.max_nodes        = 60;
        spec.edge_probability = 0.7;

        const auto g = lpq_test::random_graph(rng, spec);
        if (g.num_nodes() == 0) {
            continue;
        }
        run_all_implementations(g, 0, "dense seed=" + std::to_string(seed));
    }
}

TEST(Differential, TinyGraphExhaustiveSweep) {
    // Graphs of 1-5 nodes, every source. Tiny graphs hit the boundary paths --
    // empty queue, single element, immediate target -- that large random graphs
    // almost never reach.
    for (std::uint64_t seed = 3000; seed < 3400; ++seed) {
        Rng                       rng(seed);
        lpq_test::RandomGraphSpec spec;
        spec.min_nodes        = 1;
        spec.max_nodes        = 5;
        spec.edge_probability = 0.5;
        spec.allow_zero_weights = true;

        const auto g = lpq_test::random_graph(rng, spec);
        for (NodeId src = 0; src < g.num_nodes(); ++src) {
            run_all_implementations(g, src, "tiny seed=" + std::to_string(seed) +
                                                " source=" + std::to_string(src));
        }
    }
}

TEST(Differential, HighDegreeSweep) {
    for (std::uint64_t seed = 4000; seed < 4060; ++seed) {
        Rng                       rng(seed);
        lpq_test::RandomGraphSpec spec;
        spec.min_nodes           = 100;
        spec.max_nodes           = 200;
        spec.edge_probability    = 0.3;
        spec.extreme_weight_spread = true;

        const auto g = lpq_test::random_graph(rng, spec);
        if (g.num_nodes() == 0) {
            continue;
        }
        run_all_implementations(g, 0, "high-degree seed=" + std::to_string(seed));
    }
}

// -----------------------------------------------------------------------------
// Optimality checked against the graph, not against another implementation
// -----------------------------------------------------------------------------

TEST(Differential, EveryQueueProducesARelaxationStableResult) {
    // Catches a bug that happened to affect the reference and a candidate in
    // the same way, which agreement alone would not reveal.
    for (std::uint64_t seed = 5000; seed < 5200; ++seed) {
        Rng                       rng(seed);
        lpq_test::RandomGraphSpec spec;
        spec.many_ties = (seed % 2 == 0);
        const auto g   = lpq_test::random_graph(rng, spec);
        if (g.num_nodes() == 0) {
            continue;
        }
        const std::string ctx = "seed=" + std::to_string(seed);
        DijkstraOptions   opts;
        opts.measure_time = false;

        lpq_test::expect_no_relaxable_arc(g, dijkstra<W, lpq::BinaryHeap<Dist>>(g, 0, opts),
                                          ctx + " [binary]");
        lpq_test::expect_no_relaxable_arc(g, dijkstra<W, lpq::PairingHeap<Dist>>(g, 0, opts),
                                          ctx + " [pairing]");
        lpq_test::expect_no_relaxable_arc(g, dijkstra<W, lpq::FibonacciHeap<Dist>>(g, 0, opts),
                                          ctx + " [fibonacci]");
        lpq_test::expect_no_relaxable_arc(g, dijkstra<W, lpq::RadixHeap<Dist>>(g, 0, opts),
                                          ctx + " [radix]");
    }
}

// -----------------------------------------------------------------------------
// Floating-point weights
// -----------------------------------------------------------------------------

TEST(Differential, FloatingPointGraphs) {
    // The radix heap is correctly excluded here by radix_heap_supports<double>().
    using FW    = double;
    using FDist = lpq::DistanceType<FW>;

    for (std::uint64_t seed = 6000; seed < 6100; ++seed) {
        Rng               rng(seed);
        const std::size_t n = 30;
        std::vector<lpq::InputEdge<FW>> edges;
        for (std::size_t u = 0; u < n; ++u) {
            for (std::size_t v = 0; v < n; ++v) {
                if (u != v && rng.next_unit() < 0.2) {
                    edges.push_back({static_cast<NodeId>(u), static_cast<NodeId>(v),
                                     rng.next_double(0.001, 1000.0)});
                }
            }
        }
        lpq::GraphBuildOptions bopts;
        bopts.dataset_id = "fuzz:double";
        const auto g = lpq::GraphBuilder<FW>::build(n, std::move(edges), bopts);

        const auto      expected = reference_dijkstra(g, 0);
        DijkstraOptions opts;
        opts.measure_time = false;
        const std::string ctx = "double seed=" + std::to_string(seed);

        lpq_test::expect_results_agree(g, expected, dijkstra<FW, lpq::BinaryHeap<FDist>>(g, 0, opts),
                                       ctx + " [binary]");
        lpq_test::expect_results_agree(g, expected,
                                       dijkstra<FW, lpq::PairingHeap<FDist>>(g, 0, opts),
                                       ctx + " [pairing]");
        lpq_test::expect_results_agree(g, expected,
                                       dijkstra<FW, lpq::FibonacciHeap<FDist>>(g, 0, opts),
                                       ctx + " [fibonacci]");
    }
}

// -----------------------------------------------------------------------------
// Meta-tests: the harness must still reject wrong answers
// -----------------------------------------------------------------------------
//
// The target-mode comparison was loosened after seed 1002 to stop reporting
// differing exploration frontiers as failures. A loosened oracle risks becoming
// one that accepts anything, so these tests corrupt a known-good result and
// assert the harness still rejects it. Without them, a future over-relaxation
// of the policy would silently disable every differential test above.

TEST(DifferentialHarness, DetectsAWrongDistanceInFullSsspMode) {
    const auto g        = lpq_test::grid_graph<W>(6, 6);
    const auto expected = reference_dijkstra(g, 0);

    auto corrupted = expected;
    corrupted.dist[20] += 1;  // one node, off by one

    const auto diffs = lpq_test::compare_results(g, expected, corrupted);
    EXPECT_FALSE(diffs.empty()) << "harness accepted a wrong distance";
}

TEST(DifferentialHarness, DetectsAWrongTargetDistanceInTargetMode) {
    const auto g = lpq_test::grid_graph<W>(6, 6);
    lpq::ReferenceDijkstraOptions ropts;
    ropts.target = 35;
    const auto expected = reference_dijkstra(g, 0, ropts);

    auto corrupted = expected;
    corrupted.dist[35] += 1;

    const auto diffs = lpq_test::compare_results(g, expected, corrupted);
    EXPECT_FALSE(diffs.empty()) << "harness accepted a wrong target distance";
}

TEST(DifferentialHarness, DetectsAnUnreachableNodeClaimedReachable) {
    const auto g        = lpq_test::disconnected_graph<W>(5);
    const auto expected = reference_dijkstra(g, 0);

    auto corrupted = expected;
    corrupted.dist[7] = 3;  // node in the far component, claimed reachable

    const auto diffs = lpq_test::compare_results(g, expected, corrupted);
    EXPECT_FALSE(diffs.empty()) << "harness accepted a phantom reachable node";
}

TEST(DifferentialHarness, DetectsAWrongTerminationReasonInTargetMode) {
    const auto g = lpq_test::grid_graph<W>(5, 5);
    lpq::ReferenceDijkstraOptions ropts;
    ropts.target = 24;
    const auto expected = reference_dijkstra(g, 0, ropts);

    auto corrupted        = expected;
    corrupted.termination = lpq::SearchTermination::kTargetUnreachable;

    const auto diffs = lpq_test::compare_results(g, expected, corrupted);
    EXPECT_FALSE(diffs.empty()) << "harness accepted a wrong termination reason";
}

TEST(DifferentialHarness, DetectsAParentArrayThatDoesNotFormAnOptimalPath) {
    // The distance array stays correct while the predecessor chain is made to
    // describe a costlier route. Comparing distances alone would miss this;
    // reconstructing the path and recomputing its cost catches it.
    const auto g        = lpq_test::diamond_graph();
    const auto expected = reference_dijkstra(g, 0);

    auto corrupted = expected;
    corrupted.parent[2] = 0;  // claim 0->2 direct (cost 4) though dist[2] == 2

    const auto diffs = lpq_test::compare_results(g, expected, corrupted);
    EXPECT_FALSE(diffs.empty())
        << "harness accepted a parent array inconsistent with its distances";
}

TEST(DifferentialHarness, AcceptsAGenuinelyEquivalentResult) {
    // The complement: the harness must not reject a correct implementation that
    // merely broke ties differently. Otherwise the tests above would pass for
    // the wrong reason.
    const auto g = lpq_test::zero_weight_graph<W>(30);
    const auto a = reference_dijkstra(g, 0);
    const auto b = reference_dijkstra_heap(g, 0);

    const auto diffs = lpq_test::compare_results(g, a, b);
    EXPECT_TRUE(diffs.empty()) << lpq_test::describe(diffs);
}

TEST(Differential, RadixHeapIsExcludedForFloatingPointWeights) {
    // ESD.md section 7: incompatible configurations must be reported as
    // unsupported rather than silently producing invalid results.
    static_assert(!lpq::radix_heap_supports<double>());
    EXPECT_FALSE(std::string(lpq::radix_heap_unsupported_reason<double>()).empty());
}

}  // namespace
