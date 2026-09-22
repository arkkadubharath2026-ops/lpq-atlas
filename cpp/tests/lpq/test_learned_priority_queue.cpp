// =============================================================================
// The learned priority queue end to end (ESD.md section 39 item 19, PRD.md
// sections 4.1 and 16).
//
// PRD.md section 4.1: "The returned shortest path must be identical in cost to
// a trusted exact reference implementation." ESD.md section 24 states the same
// as an invariant: "exact mode == trusted reference".
//
// The LPQ satisfies PriorityQueueLike, so it goes through the SAME differential
// machinery that validated the five classical queues, unmodified. That is the
// strongest evidence available that the learned path is correct: nothing about
// the test was relaxed to accommodate it.
// =============================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "lpq/algorithms/dijkstra.hpp"
#include "lpq/algorithms/reference_dijkstra.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/features/graph_features.hpp"
#include "lpq/graph/graph_builder.hpp"
#include "lpq/graph/graph_metadata.hpp"
#include "lpq/lpq/learned_priority_queue.hpp"
#include "lpq/models/linear_predictor.hpp"
#include "lpq/pq/priority_queue.hpp"
#include "lpq_test/graph_fixtures.hpp"

namespace lpq {
namespace {

using Weight = std::uint32_t;
using Lpq    = LearnedPriorityQueue<Weight, ConstantNodeScorer>;
using StaticLpq =
    LearnedPriorityQueue<Weight, StaticNodeScorer<LinearPredictor>>;

// The LPQ must be usable everywhere a classical queue is, with no special
// casing in Dijkstra.
static_assert(PriorityQueueLike<Lpq>);
static_assert(LazyDeletionQueue<Lpq>);
static_assert(PriorityQueueLike<StaticLpq>);

CandidateWindowConfig window(std::uint32_t k) {
    CandidateWindowConfig c;
    c.k_min          = k;
    c.k_max          = k;
    c.max_expansions = 2;
    return c;
}

// -----------------------------------------------------------------------------
// Queue-level correctness
// -----------------------------------------------------------------------------

TEST(LearnedPriorityQueue, PopsInKeyOrder) {
    ConstantNodeScorer scorer;
    Lpq                q(4, window(4));
    q.set_scorer(&scorer);

    q.push(1, 30);
    q.push(2, 10);
    q.push(3, 20);

    EXPECT_EQ(q.pop().key, 10U);
    EXPECT_EQ(q.pop().key, 20U);
    EXPECT_EQ(q.pop().key, 30U);
    EXPECT_TRUE(q.empty());
}

TEST(LearnedPriorityQueue, WorksWithNoScorerAttached) {
    // A null scorer must degrade to the classical bucket queue rather than
    // dereferencing nothing: the queue is constructible before a graph exists,
    // and a crash there would be a trap for every caller.
    Lpq q(4, window(4));
    q.push(1, 30);
    q.push(2, 10);
    EXPECT_EQ(q.pop().key, 10U);
    EXPECT_EQ(q.pop().key, 30U);
}

TEST(LearnedPriorityQueue, MatchesAFullDrainAgainstSortedInput) {
    Rng rng(20260922U);

    for (int trial = 0; trial < 500; ++trial) {
        ConstantNodeScorer scorer;
        Lpq                q(1 + rng.next_below(32), window(1 + static_cast<std::uint32_t>(
                                                     rng.next_below(8))));
        q.set_scorer(&scorer);

        std::vector<std::uint64_t> keys;
        const std::size_t          n = 1 + static_cast<std::size_t>(rng.next_below(60));
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint64_t key = rng.next_below(500);
            q.push(static_cast<NodeId>(i), key);
            keys.push_back(key);
        }

        std::sort(keys.begin(), keys.end());
        for (std::uint64_t expected : keys) {
            ASSERT_FALSE(q.empty()) << "trial " << trial;
            EXPECT_EQ(q.pop().key, expected) << "trial " << trial;
        }
        EXPECT_TRUE(q.empty());
    }
}

TEST(LearnedPriorityQueue, NeverReportsAWindowDivergence) {
    // A non-zero count means the candidate window and the bucket disagreed
    // about what the bucket contains. Correctness survives it -- the extraction
    // falls through to the classical scan -- but it would mean a real bug.
    Rng                rng(1002U);
    ConstantNodeScorer scorer;

    for (int trial = 0; trial < 200; ++trial) {
        Lpq q(1 + rng.next_below(16), window(4));
        q.set_scorer(&scorer);

        for (NodeId u = 0; u < 60; ++u) {
            q.push(u, rng.next_below(200));
        }
        while (!q.empty()) {
            (void)q.pop();
        }
        ASSERT_EQ(q.metrics().window_divergences, 0U) << "trial " << trial;
    }
}

// -----------------------------------------------------------------------------
// Exact mode against the trusted reference (PRD.md section 4.1)
// -----------------------------------------------------------------------------

TEST(LearnedPriorityQueue, MatchesTheReferenceOnFixtureGraphs) {
    const std::vector<CsrGraph<Weight>> graphs = {
        lpq_test::grid_graph<Weight>(10, 10), lpq_test::path_graph<Weight>(50),
        lpq_test::star_graph<Weight>(40),     lpq_test::grid_graph<Weight>(1, 30),
    };

    for (std::size_t g = 0; g < graphs.size(); ++g) {
        const CsrGraph<Weight>& graph = graphs[g];

        for (NodeId source = 0; source < graph.num_nodes(); source += 7) {
            const auto expected = reference_dijkstra<Weight>(graph, source);

            ConstantNodeScorer scorer;
            Lpq                queue(4, window(4));
            queue.set_scorer(&scorer);
            const auto actual = dijkstra<Weight>(graph, source, queue);

            ASSERT_EQ(actual.dist.size(), expected.dist.size());
            for (NodeId u = 0; u < graph.num_nodes(); ++u) {
                EXPECT_EQ(actual.dist[u], expected.dist[u])
                    << "graph " << g << ", source " << source << ", node " << u;
            }
        }
    }
}

// The full randomised sweep. Every query is checked against the O(n^2)
// reference, which shares no structure with any queue in this project.
TEST(LearnedPriorityQueue, MatchesTheReferenceOnRandomGraphs) {
    Rng rng(1002U);  // the permanent differential regression seed

    for (int trial = 0; trial < 400; ++trial) {
        lpq_test::RandomGraphSpec spec;
        spec.min_nodes        = 2;
        spec.max_nodes        = 40;
        spec.edge_probability = 0.25;
        spec.max_weight       = 50;
        spec.many_ties        = (trial % 3 == 0);  // ties are the hard case here
        const CsrGraph<Weight> graph = lpq_test::random_graph(rng, spec);

        const NodeId source = rng.next_node(static_cast<NodeId>(graph.num_nodes()));
        const auto   expected = reference_dijkstra<Weight>(graph, source);

        // Bucket width and window size vary so the sweep covers both the
        // easy regime (narrow buckets, verification almost always accepts) and
        // the hard one (wide buckets, frequent fallback scans).
        const std::uint64_t width = 1 + rng.next_below(40);
        const auto          k     = static_cast<std::uint32_t>(1 + rng.next_below(6));

        ConstantNodeScorer scorer;
        Lpq                queue(width, window(k));
        queue.set_scorer(&scorer);

        const auto actual = dijkstra<Weight>(graph, source, queue);

        for (NodeId u = 0; u < graph.num_nodes(); ++u) {
            ASSERT_EQ(actual.dist[u], expected.dist[u])
                << "trial " << trial << ", width " << width << ", k " << k << ", source "
                << source << ", node " << u;
        }
    }
}

// With a real model driving the front. The scores change which candidates are
// examined, so this exercises a different set of accept/fallback paths from the
// constant-scorer sweep -- and must still be exact.
TEST(LearnedPriorityQueue, MatchesTheReferenceWithALearnedScorer) {
    Rng rng(4242U);

    LinearPredictor model;
    model.set_weight(Feature::kOutDegree, -1.0F);
    model.set_weight(Feature::kMinAdjacentWeight, -2.0F);
    model.set_weight(Feature::kMeanAdjacentWeight, 0.5F);

    for (int trial = 0; trial < 300; ++trial) {
        lpq_test::RandomGraphSpec spec;
        spec.min_nodes        = 2;
        spec.max_nodes        = 40;
        spec.edge_probability = 0.3;
        spec.max_weight       = 40;
        const CsrGraph<Weight> graph = lpq_test::random_graph(rng, spec);

        const GraphMetadata meta = compute_metadata(graph);
        GraphFeatures       features;
        features.build(graph, meta);

        StaticNodeScorer<LinearPredictor> scorer;
        scorer.build(features, model);
        ASSERT_EQ(scorer.size(), graph.num_nodes());

        const NodeId source   = rng.next_node(static_cast<NodeId>(graph.num_nodes()));
        const auto   expected = reference_dijkstra<Weight>(graph, source);

        StaticLpq queue(1 + rng.next_below(30), window(4));
        queue.set_scorer(&scorer);

        const auto actual = dijkstra<Weight>(graph, source, queue);

        for (NodeId u = 0; u < graph.num_nodes(); ++u) {
            ASSERT_EQ(actual.dist[u], expected.dist[u])
                << "trial " << trial << ", source " << source << ", node " << u;
        }
    }
}

TEST(LearnedPriorityQueue, MatchesTheReferenceOnAnUnweightedGraph) {
    // Every frontier node shares a key, so the whole frontier lands in one
    // bucket. This is the worst case for the scan and the best case for a
    // correct prediction, and it is where an off-by-one in the front would
    // show.
    const CsrGraph<Weight> graph = lpq_test::grid_graph<Weight>(12, 12, /*weight=*/1);
    const auto             expected = reference_dijkstra<Weight>(graph, 0);

    ConstantNodeScorer scorer;
    Lpq                queue(8, window(4));
    queue.set_scorer(&scorer);
    const auto actual = dijkstra<Weight>(graph, 0, queue);

    for (NodeId u = 0; u < graph.num_nodes(); ++u) {
        EXPECT_EQ(actual.dist[u], expected.dist[u]) << "node " << u;
    }
}

// -----------------------------------------------------------------------------
// The saving is real, and it is measured rather than assumed
// -----------------------------------------------------------------------------

TEST(LearnedPriorityQueue, ReportsHowManyEntriesItAvoidedTouching) {
    // A wide bucket with many equal-keyed entries: verification should accept
    // from the front repeatedly and leave most of the bucket untouched.
    ConstantNodeScorer scorer;
    Lpq                q(64, window(4));
    q.set_scorer(&scorer);

    for (NodeId u = 0; u < 200; ++u) {
        q.push(u, 5);  // all identical, all in one bucket
    }
    while (!q.empty()) {
        (void)q.pop();
    }

    const LpqMetrics& m = q.metrics();
    EXPECT_EQ(m.extractions, 200U);
    EXPECT_GT(m.verified_extractions, 0U)
        << "no extraction avoided the scan; the learned path never engaged";
    EXPECT_GT(m.entries_skipped, 0U);
    EXPECT_EQ(m.window_divergences, 0U);
}

TEST(LearnedPriorityQueue, FallsBackWhenTheFrontCannotSatisfyTheBound) {
    // A wide bucket whose minimum sits outside the front. The queue must still
    // return the true minimum, via the fallback scan.
    ConstantNodeScorer scorer;
    Lpq                q(1000, window(2));
    q.set_scorer(&scorer);

    for (NodeId u = 0; u < 50; ++u) {
        q.push(u, 900 - u);  // the last pushed holds the smallest key
    }

    EXPECT_EQ(q.pop().key, 851U);
    EXPECT_GT(q.metrics().fallback_scans, 0U);
}

TEST(LearnedPriorityQueue, CountsVerificationComparisonsAsQueueWork) {
    // Reporting only the substrate's comparisons would understate the learned
    // queue against the classical ones by exactly the cost of the learned
    // machinery, which is the comparison the project turns on.
    ConstantNodeScorer scorer;
    Lpq                q(64, window(4));
    q.set_scorer(&scorer);

    for (NodeId u = 0; u < 50; ++u) {
        q.push(u, 5);
    }
    q.reset_comparison_count();
    (void)q.pop();

    EXPECT_GT(q.comparison_count(), 0U);
    EXPECT_GT(q.verification_metrics().comparisons, 0U);
}

// Resetting a performance counter must not destroy the experiment's primary
// results. Dijkstra calls reset_comparison_count() at the start of every query.
TEST(LearnedPriorityQueue, ResettingComparisonsPreservesVerificationStatistics) {
    ConstantNodeScorer scorer;
    Lpq                q(64, window(4));
    q.set_scorer(&scorer);

    for (NodeId u = 0; u < 20; ++u) {
        q.push(u, 5);
    }
    (void)q.pop();
    const std::uint64_t accepted_before = q.verification_metrics().accepted;
    ASSERT_GT(accepted_before, 0U);

    q.reset_comparison_count();

    EXPECT_EQ(q.verification_metrics().accepted, accepted_before);
    EXPECT_EQ(q.comparison_count(), 0U);
}

}  // namespace
}  // namespace lpq
