// =============================================================================
// Adaptation driven by a live search (ESD.md section 40: "online updates work").
//
// The controller and the learner were unit-tested against synthetic event
// streams long before anything could run them: neither had a caller outside its
// own tests. Those tests establish that the components behave correctly when
// fed events. They cannot establish that events ever arrive.
//
// These tests run a real Dijkstra over a real graph with the observer attached,
// and assert the things that distinguish "wired up" from "compiles":
//
//   * the observer is actually called, once per extraction;
//   * a retreat actually changes what the queue does;
//   * the learner receives events whose labels came from the verifier;
//   * and, most importantly, the search still returns correct answers with all
//     of it attached -- adaptation is a performance hint and must never be able
//     to make the queue wrong.
// =============================================================================
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "lpq/adaptation/controller_observer.hpp"
#include "lpq/algorithms/dijkstra.hpp"
#include "lpq/algorithms/reference_dijkstra.hpp"
#include "lpq/graph/graph_builder.hpp"
#include "lpq/lpq/learned_priority_queue.hpp"
#include "lpq/common/rng.hpp"
#include "lpq_test/graph_fixtures.hpp"

namespace lpq {
namespace {

using Weight = std::uint32_t;
using Lpq    = LearnedPriorityQueue<Weight, ConstantNodeScorer>;

CandidateWindowConfig window(std::uint32_t k) {
    CandidateWindowConfig c;
    c.k_min = k;
    c.k_max = k;
    return c;
}

CsrGraph<Weight> grid(std::uint32_t side, Weight w = 7) {
    std::vector<InputEdge<Weight>> edges;
    for (std::uint32_t r = 0; r < side; ++r) {
        for (std::uint32_t c = 0; c < side; ++c) {
            const NodeId id = r * side + c;
            if (c + 1 < side) {
                edges.push_back({id, id + 1, w});
            }
            if (r + 1 < side) {
                edges.push_back({id, id + side, w});
            }
        }
    }
    GraphBuildOptions opts;
    opts.directedness = Directedness::kUndirected;
    return GraphBuilder<Weight>::build(side * side, std::move(edges), opts);
}

/// Counts calls, so "was it wired up" is answerable separately from "did it
/// behave well".
class CountingObserver final : public ExtractionObserver {
public:
    explicit CountingObserver(ExtractionPolicy policy) : policy_(policy) {}

    [[nodiscard]] ExtractionPolicy policy() const noexcept override {
        ++policy_calls;
        return policy_;
    }

    void on_extraction(std::uint64_t skipped, std::uint32_t comparisons,
                       const CorrectionOutcome& outcome) noexcept override {
        ++extractions;
        total_skipped += skipped;
        total_comparisons += comparisons;
        if (outcome.path == ResolutionPath::kFallback) {
            ++fallbacks;
        }
    }

    mutable std::uint64_t policy_calls = 0;
    std::uint64_t extractions          = 0;
    std::uint64_t total_skipped        = 0;
    std::uint64_t total_comparisons    = 0;
    std::uint64_t fallbacks            = 0;

private:
    ExtractionPolicy policy_;
};

// -----------------------------------------------------------------------------
// The seam is actually reached
// -----------------------------------------------------------------------------

TEST(ControllerObserver, TheQueueConsultsAndReportsOncePerExtraction) {
    const CsrGraph<Weight> g = grid(12);
    CountingObserver       observer{ExtractionPolicy{}};

    ConstantNodeScorer scorer;
    Lpq                queue(4, window(4));
    queue.set_scorer(&scorer);
    queue.set_observer(&observer);

    const auto result = dijkstra<Weight>(g, 0, queue);

    EXPECT_GT(observer.extractions, 0U);
    // One policy query and one report per pop. If these diverge the queue is
    // taking a path that reports without asking, or asks without reporting.
    EXPECT_EQ(observer.policy_calls, observer.extractions);
    EXPECT_EQ(observer.extractions, queue.metrics().extractions);
    EXPECT_TRUE(result.reachable(g.num_nodes() - 1));
}

TEST(ControllerObserver, AQueueWithNoObserverIsUnaffected) {
    // The baseline must not pay for a feature it does not use, and must produce
    // identical work. Comparison counts are exact, so this is an equality.
    const CsrGraph<Weight> g = grid(12);

    ConstantNodeScorer scorer;
    Lpq                plain(4, window(4));
    plain.set_scorer(&scorer);
    const auto a = dijkstra<Weight>(g, 0, plain);
    const std::uint64_t plain_comparisons = plain.comparison_count();

    CountingObserver observer{ExtractionPolicy{}};
    Lpq              watched(4, window(4));
    watched.set_scorer(&scorer);
    watched.set_observer(&observer);
    const auto b = dijkstra<Weight>(g, 0, watched);

    EXPECT_EQ(a.dist, b.dist);
    // Watching must not change the work, only observe it.
    EXPECT_EQ(plain_comparisons, watched.comparison_count());
}

// -----------------------------------------------------------------------------
// A retreat changes what the queue does
// -----------------------------------------------------------------------------

TEST(ControllerObserver, RetreatingSendsEveryExtractionDownTheClassicalPath) {
    // The controller's retreat position. If use_learned_path did nothing, this
    // would be indistinguishable from the learned path and the whole adaptive
    // mechanism would be decorative.
    const CsrGraph<Weight> g = grid(12);

    ExtractionPolicy retreat;
    retreat.use_learned_path = false;
    CountingObserver observer{retreat};

    ConstantNodeScorer scorer;
    Lpq                queue(4, window(4));
    queue.set_scorer(&scorer);
    queue.set_observer(&observer);

    const auto result = dijkstra<Weight>(g, 0, queue);

    EXPECT_EQ(queue.metrics().fallback_scans, queue.metrics().extractions);
    EXPECT_EQ(queue.metrics().verified_extractions, 0U);
    // Still correct. That is the point of the fallback being the classical
    // queue rather than a degraded approximation of it.
    const auto reference = reference_dijkstra<Weight>(g, 0);
    EXPECT_EQ(result.dist, reference.dist);
}

TEST(ControllerObserver, ARetreatIsReportedAsSkippingNothing) {
    // Zero entries skipped is the signal the controller uses to notice a
    // retreat is not free. Reporting silence instead would let it conclude the
    // retreat cost nothing and never come back.
    const CsrGraph<Weight> g = grid(10);

    ExtractionPolicy retreat;
    retreat.use_learned_path = false;
    CountingObserver observer{retreat};

    ConstantNodeScorer scorer;
    Lpq                queue(4, window(4));
    queue.set_scorer(&scorer);
    queue.set_observer(&observer);
    (void)dijkstra<Weight>(g, 0, queue);

    EXPECT_GT(observer.extractions, 0U);
    EXPECT_EQ(observer.total_skipped, 0U);
}

// -----------------------------------------------------------------------------
// The controller and learner actually receive the events
// -----------------------------------------------------------------------------

TEST(ControllerObserver, TheControllerObservesEveryExtraction) {
    const CsrGraph<Weight> g = grid(14);

    ControllerConfig config;
    config.warmup_extractions = 8;
    AdaptiveController controller(config);
    ControllerObserver observer(controller);

    ConstantNodeScorer scorer;
    Lpq                queue(4, window(4));
    queue.set_scorer(&scorer);
    queue.set_observer(&observer);
    (void)dijkstra<Weight>(g, 0, queue);

    EXPECT_EQ(controller.metrics().observations, queue.metrics().extractions);
    EXPECT_GT(controller.metrics().observations, 0U);
}

TEST(ControllerObserver, TheLearnerReceivesEventsFromRealExtractions) {
    const CsrGraph<Weight> g = grid(14);

    AdaptiveController controller{ControllerConfig{}};
    OnlineLearner      learner{OnlineLearningConfig{}};
    ControllerObserver observer(controller, &learner);

    ConstantNodeScorer scorer;
    Lpq                queue(4, window(4));
    queue.set_scorer(&scorer);
    queue.set_observer(&observer);
    (void)dijkstra<Weight>(g, 0, queue);

    EXPECT_EQ(learner.metrics().events_recorded, queue.metrics().extractions);
    EXPECT_GT(learner.metrics().events_recorded, 0U);
}

TEST(ControllerObserver, ThePolicyMirrorsTheControllersOwn) {
    // The two policy structs are separate types on purpose. A field added to
    // one and forgotten in the other would silently stop being honoured.
    AdaptiveController controller{ControllerConfig{}};
    ControllerObserver observer(controller);

    const QueuePolicy     mine   = controller.policy();
    const ExtractionPolicy theirs = observer.policy();

    EXPECT_EQ(mine.use_learned_path, theirs.use_learned_path);
    EXPECT_EQ(mine.window_size, theirs.window_size);
    EXPECT_EQ(mine.max_expansions, theirs.max_expansions);
}

// -----------------------------------------------------------------------------
// Correctness is never negotiable
// -----------------------------------------------------------------------------

TEST(ControllerObserver, AdaptationNeverMakesTheAnswerWrong) {
    // Across random graphs, with the controller free to switch modes whenever
    // it likes. Adaptation is a performance hint; if any mode it can select
    // produces a wrong distance, the hint has become a correctness bug.
    Rng rng(20260924U);

    for (int trial = 0; trial < 60; ++trial) {
        lpq_test::RandomGraphSpec spec;
        spec.min_nodes        = 4;
        spec.max_nodes        = 70;
        spec.edge_probability = 0.25;
        spec.max_weight       = 60;
        const CsrGraph<Weight> g = lpq_test::random_graph(rng, spec);
        const NodeId source      = rng.next_node(static_cast<NodeId>(g.num_nodes()));

        ControllerConfig config;
        // Almost no warmup and no dwell, so the controller changes mode as
        // often as it can. The point is to exercise every mode inside one
        // search rather than to be realistic.
        config.warmup_extractions   = 1;
        config.dwell_extractions = 1;
        AdaptiveController controller(config);
        OnlineLearner      learner{OnlineLearningConfig{}};
        ControllerObserver observer(controller, &learner);

        ConstantNodeScorer scorer;
        Lpq                queue(1 + rng.next_below(20), window(2 + rng.next_below(6)));
        queue.set_scorer(&scorer);
        queue.set_observer(&observer);

        const auto actual   = dijkstra<Weight>(g, source, queue);
        const auto expected = reference_dijkstra<Weight>(g, source);

        ASSERT_EQ(actual.dist, expected.dist)
            << "trial " << trial << " disagreed with the reference with adaptation active";
    }
}

}  // namespace
}  // namespace lpq
