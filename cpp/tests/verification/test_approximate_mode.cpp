// =============================================================================
// Bounded approximate mode (PRD.md sections 4.2 and 16, ESD.md section 17).
//
// The whole point of this file is to MEASURE the approximation rather than
// assume it. PRD.md section 16 requires the exact optimum to be computed with
// the reference implementation and the ratio returned_cost / optimal_cost to be
// reported; that is what happens here, and the observed ratios are printed so
// they enter the experimental record instead of being reduced to a pass mark.
//
// The end-to-end bound is NOT proven in this codebase (see
// correctness_policy.hpp), so no test here asserts that the measured ratio
// stays inside the configured epsilon. What IS asserted is everything that
// would be wrong at any epsilon: reachability must match exactly, no returned
// cost may be below the optimum, and no path may be invented or lost.
// =============================================================================
#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <vector>

#include "lpq/algorithms/dijkstra.hpp"
#include "lpq/algorithms/reference_dijkstra.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/lpq/learned_priority_queue.hpp"
#include "lpq/verification/approximation_report.hpp"
#include "lpq_test/graph_fixtures.hpp"

namespace lpq {
namespace {

using Weight = std::uint32_t;
using Lpq    = LearnedPriorityQueue<Weight, ConstantNodeScorer>;

CandidateWindowConfig window(std::uint32_t k) {
    CandidateWindowConfig c;
    c.k_min          = k;
    c.k_max          = k;
    c.max_expansions = 1;
    return c;
}

// -----------------------------------------------------------------------------
// The report itself
// -----------------------------------------------------------------------------

TEST(ApproximationReport, AnExactRunMeasuresRatioOne) {
    const std::vector<DistanceType<Weight>> returned = {0, 10, 20, 30};
    const std::vector<DistanceType<Weight>> optimal  = {0, 10, 20, 30};

    const ApproximationReport r = measure_approximation<Weight>(returned, optimal, 0.0);
    EXPECT_EQ(r.compared, 4U);
    EXPECT_NEAR(r.max_ratio, 1.0, 1e-12);
    EXPECT_NEAR(r.median_ratio, 1.0, 1e-12);
    EXPECT_FALSE(r.exceeded_budget);
    EXPECT_TRUE(r.correctness_intact());
}

TEST(ApproximationReport, ComputesTheRatioAgainstTheOptimum) {
    const std::vector<DistanceType<Weight>> returned = {0, 11, 20};
    const std::vector<DistanceType<Weight>> optimal  = {0, 10, 20};

    const ApproximationReport r = measure_approximation<Weight>(returned, optimal, 0.2);
    EXPECT_NEAR(r.max_ratio, 1.1, 1e-12);
    EXPECT_FALSE(r.exceeded_budget);  // 1.1 <= 1.2
}

// A run over budget is reported, never failed. The end-to-end bound is not
// proven, so exceeding it is a finding about the method -- and failing here
// would suppress the evidence that decides the question.
TEST(ApproximationReport, ReportsRatherThanHidesABudgetOverrun) {
    const std::vector<DistanceType<Weight>> returned = {0, 30};
    const std::vector<DistanceType<Weight>> optimal  = {0, 10};

    const ApproximationReport r = measure_approximation<Weight>(returned, optimal, 0.1);
    EXPECT_NEAR(r.max_ratio, 3.0, 1e-12);
    EXPECT_TRUE(r.exceeded_budget);
    // Still correct in the sense that matters: nothing was invented or lost.
    EXPECT_TRUE(r.correctness_intact());
}

TEST(ApproximationReport, TreatsALostPathAsACorrectnessFailureAtAnyEpsilon) {
    const std::vector<DistanceType<Weight>> returned = {0, kUnreachable<Weight>};
    const std::vector<DistanceType<Weight>> optimal  = {0, 10};

    const ApproximationReport r = measure_approximation<Weight>(returned, optimal, 10.0);
    EXPECT_EQ(r.reachability_mismatches, 1U);
    EXPECT_FALSE(r.correctness_intact());
}

TEST(ApproximationReport, TreatsAnInventedPathAsACorrectnessFailure) {
    const std::vector<DistanceType<Weight>> returned = {0, 10};
    const std::vector<DistanceType<Weight>> optimal  = {0, kUnreachable<Weight>};

    const ApproximationReport r = measure_approximation<Weight>(returned, optimal, 10.0);
    EXPECT_EQ(r.reachability_mismatches, 1U);
    EXPECT_FALSE(r.correctness_intact());
}

// Cheaper than optimal is not a good approximation -- it means one of the two
// implementations is wrong.
TEST(ApproximationReport, FlagsACostBelowTheOptimum) {
    const std::vector<DistanceType<Weight>> returned = {0, 5};
    const std::vector<DistanceType<Weight>> optimal  = {0, 10};

    const ApproximationReport r = measure_approximation<Weight>(returned, optimal, 1.0);
    EXPECT_EQ(r.below_optimal, 1U);
    EXPECT_FALSE(r.correctness_intact());
}

TEST(ApproximationReport, ExcludesMutuallyUnreachableTargetsFromTheRatios) {
    // Including them as 1.0 would make a bad approximation look better the more
    // disconnected the graph was.
    const std::vector<DistanceType<Weight>> returned = {0, 20, kUnreachable<Weight>,
                                                        kUnreachable<Weight>};
    const std::vector<DistanceType<Weight>> optimal  = {0, 10, kUnreachable<Weight>,
                                                       kUnreachable<Weight>};

    const ApproximationReport r = measure_approximation<Weight>(returned, optimal, 1.0);
    EXPECT_EQ(r.both_unreachable, 2U);
    EXPECT_EQ(r.compared, 2U);  // the source and the one real target
    EXPECT_NEAR(r.max_ratio, 2.0, 1e-12);
}

TEST(ApproximationReport, FlagsAPositiveCostWhereZeroWasOptimal) {
    const std::vector<DistanceType<Weight>> returned = {5};
    const std::vector<DistanceType<Weight>> optimal  = {0};

    const ApproximationReport r = measure_approximation<Weight>(returned, optimal, 1.0);
    EXPECT_EQ(r.zero_cost_mismatches, 1U);
    EXPECT_FALSE(r.correctness_intact());
}

// -----------------------------------------------------------------------------
// Approximate mode end to end
// -----------------------------------------------------------------------------

struct SweepResult {
    ApproximationReport report;
    std::uint64_t       fallbacks   = 0;
    std::uint64_t       extractions = 0;
};

/// Runs one epsilon across a fixed set of random graphs and aggregates.
SweepResult run_sweep(double epsilon, std::uint32_t seed) {
    Rng rng(seed);

    SweepResult out;
    std::vector<double> all_ratios;

    for (int trial = 0; trial < 120; ++trial) {
        lpq_test::RandomGraphSpec spec;
        spec.min_nodes        = 4;
        spec.max_nodes        = 60;
        spec.edge_probability = 0.2;
        spec.max_weight       = 100;
        const CsrGraph<Weight> graph = lpq_test::random_graph(rng, spec);

        const NodeId source   = rng.next_node(static_cast<NodeId>(graph.num_nodes()));
        const auto   expected = reference_dijkstra<Weight>(graph, source);

        const CorrectnessPolicy policy =
            epsilon == 0.0 ? CorrectnessPolicy::exact() : CorrectnessPolicy::approximate(epsilon);

        ConstantNodeScorer scorer;
        Lpq                queue(1 + rng.next_below(30), window(2), policy);
        queue.set_scorer(&scorer);

        const auto actual = dijkstra<Weight>(graph, source, queue);

        const ApproximationReport r =
            measure_approximation<Weight>(actual.dist, expected.dist, epsilon);

        out.report.compared += r.compared;
        out.report.both_unreachable += r.both_unreachable;
        out.report.reachability_mismatches += r.reachability_mismatches;
        out.report.zero_cost_mismatches += r.zero_cost_mismatches;
        out.report.below_optimal += r.below_optimal;
        out.report.max_ratio = std::max(out.report.max_ratio, r.max_ratio);
        out.report.exceeded_budget = out.report.exceeded_budget || r.exceeded_budget;

        out.fallbacks += queue.metrics().fallback_scans;
        out.extractions += queue.metrics().extractions;
    }

    out.report.configured_epsilon = epsilon;
    return out;
}

// Exact mode must measure a ratio of exactly 1 everywhere. This is the control
// that gives the approximate numbers below their meaning.
TEST(ApproximateMode, ExactModeMeasuresRatioExactlyOne) {
    const SweepResult r = run_sweep(/*epsilon=*/0.0, 1002U);

    EXPECT_GT(r.report.compared, 0U);
    EXPECT_TRUE(r.report.correctness_intact());
    EXPECT_EQ(r.report.max_ratio, 1.0)
        << "exact mode returned a non-optimal path; the max ratio was " << r.report.max_ratio;
}

// The measurement PRD.md section 16 asks for, across the epsilon sweep. No
// assertion is made that the ratio stays inside the budget -- that bound is not
// proven -- but everything that would be wrong at any epsilon is asserted.
TEST(ApproximateMode, MeasuresApproximationRatiosAcrossAnEpsilonSweep) {
    for (double epsilon : {0.01, 0.1, 0.5, 2.0}) {
        const SweepResult r = run_sweep(epsilon, 1002U);

        std::cout << "[  MEASURED ] epsilon " << epsilon << ": max ratio " << r.report.max_ratio
                  << ", compared " << r.report.compared << ", fallback rate "
                  << (r.extractions == 0
                          ? 0.0
                          : static_cast<double>(r.fallbacks) /
                                static_cast<double>(r.extractions))
                  << (r.report.exceeded_budget ? "  [EXCEEDED BUDGET]" : "") << std::endl;

        // Asserted: nothing that would be wrong at ANY epsilon.
        EXPECT_EQ(r.report.reachability_mismatches, 0U) << "epsilon " << epsilon;
        EXPECT_EQ(r.report.below_optimal, 0U) << "epsilon " << epsilon;
        EXPECT_EQ(r.report.zero_cost_mismatches, 0U) << "epsilon " << epsilon;
        EXPECT_GT(r.report.compared, 0U) << "epsilon " << epsilon;

        // Not asserted: that max_ratio <= 1 + epsilon. See the file header.
    }
}

// The reason to run approximate mode at all: the slack should let verification
// accept where exact mode refused, reducing fallbacks. If it does not, the mode
// costs correctness for nothing.
TEST(ApproximateMode, ALargerEpsilonDoesNotIncreaseTheFallbackRate) {
    const SweepResult exact  = run_sweep(0.0, 4242U);
    const SweepResult loose  = run_sweep(2.0, 4242U);

    const double exact_rate = static_cast<double>(exact.fallbacks) /
                              static_cast<double>(exact.extractions);
    const double loose_rate = static_cast<double>(loose.fallbacks) /
                              static_cast<double>(loose.extractions);

    std::cout << "[  MEASURED ] fallback rate: exact " << exact_rate << ", epsilon 2.0 "
              << loose_rate << std::endl;

    EXPECT_LE(loose_rate, exact_rate + 1e-9)
        << "the approximation slack failed to reduce fallbacks, so the mode is trading "
           "correctness for nothing";
}

TEST(ApproximateMode, ReachabilityIsNeverApproximated) {
    // Whatever epsilon is configured, the set of reachable nodes must match the
    // reference exactly. An epsilon is a budget on cost, not on connectivity.
    Rng rng(20260922U);

    for (double epsilon : {0.5, 5.0, 100.0}) {
        for (int trial = 0; trial < 60; ++trial) {
            lpq_test::RandomGraphSpec spec;
            spec.min_nodes        = 4;
            spec.max_nodes        = 40;
            spec.edge_probability = 0.08;  // sparse, so many nodes are unreachable
            spec.max_weight       = 60;
            const CsrGraph<Weight> graph = lpq_test::random_graph(rng, spec);

            const NodeId source   = rng.next_node(static_cast<NodeId>(graph.num_nodes()));
            const auto   expected = reference_dijkstra<Weight>(graph, source);

            ConstantNodeScorer scorer;
            Lpq queue(8, window(2), CorrectnessPolicy::approximate(epsilon));
            queue.set_scorer(&scorer);
            const auto actual = dijkstra<Weight>(graph, source, queue);

            for (NodeId u = 0; u < graph.num_nodes(); ++u) {
                const bool got_reachable  = actual.dist[u] != kUnreachable<Weight>;
                const bool want_reachable = expected.dist[u] != kUnreachable<Weight>;
                ASSERT_EQ(got_reachable, want_reachable)
                    << "epsilon " << epsilon << ", trial " << trial << ", node " << u;
            }
        }
    }
}

TEST(ApproximateMode, NeverReturnsBetterThanOptimal) {
    // A cost below the reference optimum would mean the reference or the search
    // is wrong, which no epsilon can excuse.
    Rng rng(777U);

    for (int trial = 0; trial < 200; ++trial) {
        lpq_test::RandomGraphSpec spec;
        spec.min_nodes        = 4;
        spec.max_nodes        = 50;
        spec.edge_probability = 0.25;
        spec.max_weight       = 80;
        const CsrGraph<Weight> graph = lpq_test::random_graph(rng, spec);

        const NodeId source   = rng.next_node(static_cast<NodeId>(graph.num_nodes()));
        const auto   expected = reference_dijkstra<Weight>(graph, source);

        ConstantNodeScorer scorer;
        Lpq queue(16, window(3), CorrectnessPolicy::approximate(1.0));
        queue.set_scorer(&scorer);
        const auto actual = dijkstra<Weight>(graph, source, queue);

        for (NodeId u = 0; u < graph.num_nodes(); ++u) {
            if (expected.dist[u] == kUnreachable<Weight>) {
                continue;
            }
            ASSERT_GE(actual.dist[u], expected.dist[u])
                << "trial " << trial << ", node " << u;
        }
    }
}

}  // namespace
}  // namespace lpq
