// =============================================================================
// Prediction events, version 2 (M5-O2): what the queue exposes, and how labels are made.
//
// The learner can only be as good as its examples. These tests pin down that
//   * the queue reports which node the model ranked first and which node came out, on every path
//     that makes a prediction, and nothing when no prediction was made;
//   * the label is decided on KEYS the verifier compared (so a tie is not a mistake);
//   * a wrong first choice yields a negative for it AND a positive for the node that was extracted,
//     each with the feature vector the scorer used;
//   * a fallback weighs more only when the first choice was in fact worse.
// =============================================================================
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "lpq/adaptation/controller_observer.hpp"
#include "lpq/adaptation/online_learner.hpp"
#include "lpq/algorithms/dijkstra.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/features/graph_features.hpp"
#include "lpq/graph/graph_metadata.hpp"
#include "lpq/lpq/learned_priority_queue.hpp"
#include "lpq/lpq/online_node_scorer.hpp"
#include "lpq/models/logistic_predictor.hpp"
#include "lpq_test/graph_fixtures.hpp"

namespace lpq {
namespace {

using Weight = std::uint32_t;

CandidateWindowConfig window(std::uint32_t lo, std::uint32_t hi) {
    CandidateWindowConfig c;
    c.k_min = lo;
    c.k_max = hi;
    return c;
}

CorrectionOutcome outcome(ResolutionPath path, NodeId predicted, double pkey, NodeId extracted,
                          double ekey) {
    CorrectionOutcome o;
    o.path           = path;
    o.predicted_node = predicted;
    o.predicted_key  = pkey;
    o.extracted_node = extracted;
    o.extracted_key  = ekey;
    return o;
}

/// Features that identify a node, so a swapped vector is visible.
class IdentityFeatures final : public FeatureSource {
public:
    void features_of(NodeId u, FeatureVector& out) const noexcept override {
        out.fill(0.0F);
        out[0] = static_cast<float>(u) + 1.0F;
    }
};

// -----------------------------------------------------------------------------
// The label rule, on hand-built outcomes
// -----------------------------------------------------------------------------

TEST(VersionedEvent, AFirstChoiceThatWasTheMinimumIsOnePositiveExample) {
    IdentityFeatures f;
    PredictionEvent  e;
    ASSERT_TRUE(make_versioned_event(outcome(ResolutionPath::kFirstAttempt, 7, 10.0, 7, 10.0), 3, f, e));
    EXPECT_TRUE(e.was_correct);
    EXPECT_FALSE(e.has_accepted_example);
    EXPECT_EQ(e.example_count(), 1U);
    EXPECT_EQ(e.predicted_node, 7U);
    EXPECT_EQ(e.features[0], 8.0F);
    EXPECT_EQ(e.to_example().target, 1.0F);
}

TEST(VersionedEvent, ATieOnKeysIsNotAMistakeEvenForAnotherNode) {
    // The verifier may accept a different node with the same key; the first choice was still a
    // minimum, so it is a positive example and no second example is made.
    IdentityFeatures f;
    PredictionEvent  e;
    ASSERT_TRUE(make_versioned_event(outcome(ResolutionPath::kFirstAttemptCorrected, 7, 10.0, 9, 10.0), 3, f, e));
    EXPECT_TRUE(e.was_correct);
    EXPECT_EQ(e.example_count(), 1U);
}

TEST(VersionedEvent, AStrictlyWorseFirstChoiceGivesANegativeAndAPositiveForTheExtractedNode) {
    IdentityFeatures f;
    PredictionEvent  e;
    ASSERT_TRUE(make_versioned_event(outcome(ResolutionPath::kFirstAttemptCorrected, 7, 12.0, 9, 10.0), 3, f, e));
    EXPECT_FALSE(e.was_correct);
    ASSERT_TRUE(e.has_accepted_example);
    EXPECT_EQ(e.example_count(), 2U);

    const TrainingExample negative = e.to_example();
    EXPECT_EQ(negative.target, 0.0F);
    EXPECT_EQ(negative.features[0], 8.0F) << "the negative carries the FIRST CHOICE's features";

    const TrainingExample positive = e.accepted_example();
    EXPECT_EQ(positive.target, 1.0F);
    EXPECT_EQ(positive.features[0], 10.0F) << "the positive carries the EXTRACTED node's features";
    EXPECT_EQ(e.accepted_node, 9U);
}

TEST(VersionedEvent, AFallbackWeighsMoreOnlyWhenTheFirstChoiceWasWorse) {
    IdentityFeatures f;
    PredictionEvent  worse;
    PredictionEvent  tied;
    ASSERT_TRUE(make_versioned_event(outcome(ResolutionPath::kFallback, 7, 12.0, 9, 10.0), 40, f, worse));
    ASSERT_TRUE(make_versioned_event(outcome(ResolutionPath::kFallback, 7, 10.0, 9, 10.0), 40, f, tied));
    EXPECT_FALSE(worse.was_correct);
    EXPECT_EQ(worse.to_example(4.0F).weight, 4.0F);
    EXPECT_TRUE(tied.was_correct);
    EXPECT_EQ(tied.to_example(4.0F).weight, 1.0F) << "a first choice that was a minimum is not penalised";
}

TEST(VersionedEvent, NoPredictionMeansNoEvent) {
    IdentityFeatures f;
    PredictionEvent  e;
    EXPECT_FALSE(make_versioned_event(CorrectionOutcome{}, 0, f, e)) << "controller retreat";
    CorrectionOutcome empty = outcome(ResolutionPath::kEmpty, kInvalidNode, 0.0, 3, 1.0);
    EXPECT_FALSE(make_versioned_event(empty, 0, f, e)) << "no first choice was ever ranked";
}

// -----------------------------------------------------------------------------
// What the queue actually reports
// -----------------------------------------------------------------------------

/// The feature source for a bare scorer (the production one is OnlineSession).
class ScorerFeatures final : public FeatureSource {
public:
    explicit ScorerFeatures(const OnlineNodeScorer<LogisticPredictor>& scorer) : scorer_(scorer) {}
    void features_of(NodeId u, FeatureVector& out) const noexcept override {
        scorer_.features_of(u, out);
    }

private:
    const OnlineNodeScorer<LogisticPredictor>& scorer_;
};

/// Records every outcome the queue reports and builds events from them, as the real observer does.
class CapturingObserver final : public ExtractionObserver {
public:
    explicit CapturingObserver(const FeatureSource& features) : features_(features) {}

    [[nodiscard]] ExtractionPolicy policy() const noexcept override {
        ExtractionPolicy p;
        p.window_size = std::numeric_limits<std::uint32_t>::max();
        return p;
    }

    void on_extraction(std::uint64_t, std::uint32_t comparisons,
                       const CorrectionOutcome& o) noexcept override {
        outcomes.push_back(o);
        PredictionEvent e;
        if (make_versioned_event(o, comparisons, features_, e)) {
            events.push_back(e);
        }
    }

    std::vector<CorrectionOutcome> outcomes;
    std::vector<PredictionEvent>   events;

private:
    const FeatureSource& features_;
};

class QueueIdentity : public ::testing::TestWithParam<ExtractionStrategy> {};

TEST_P(QueueIdentity, EveryPredictedExtractionNamesTheFirstChoiceAndTheReturnedNode) {
    Rng rng(20261013U);
    std::uint64_t predictions = 0;
    std::uint64_t wrong       = 0;

    for (int trial = 0; trial < 25; ++trial) {
        lpq_test::RandomGraphSpec spec;
        spec.min_nodes        = 8;
        spec.max_nodes        = 60;
        spec.edge_probability = 0.25;
        spec.max_weight       = 40;
        spec.many_ties        = (trial % 3 == 0);
        const CsrGraph<Weight> g = lpq_test::random_graph(rng, spec);
        const auto meta          = compute_metadata(g);
        const GraphFeatures features(g, meta);

        // A model with distinct weights, so the front is genuinely re-ordered by score.
        LogisticPredictor model;
        model.set_weight(Feature::kOutDegree, 2.0F);
        model.set_weight(Feature::kMeanAdjacentWeight, -1.5F);
        OnlineNodeScorer<LogisticPredictor> scorer(features, model);

        using Q = LearnedPriorityQueue<Weight, OnlineNodeScorer<LogisticPredictor>>;
        Q queue(1 + rng.next_below(12), window(2, 2 + static_cast<std::uint32_t>(rng.next_below(10))),
                CorrectnessPolicy::exact(), GetParam());
        queue.set_scorer(&scorer);
        const ScorerFeatures feature_source(scorer);
        CapturingObserver observer(feature_source);
        queue.set_observer(&observer);

        const NodeId source = rng.next_node(static_cast<NodeId>(g.num_nodes()));
        (void)dijkstra<Weight>(g, source, queue);

        double last_key = -1.0;
        for (const CorrectionOutcome& o : observer.outcomes) {
            if (o.predicted_node == kInvalidNode) {
                continue;
            }
            ++predictions;
            ASSERT_NE(o.extracted_node, kInvalidNode) << "trial " << trial;
            ASSERT_LT(o.predicted_node, g.num_nodes());
            ASSERT_LT(o.extracted_node, g.num_nodes());
            // Exact Dijkstra extracts in nondecreasing key order, and the first choice's key can
            // never beat the extracted (minimum) key.
            EXPECT_GE(o.extracted_key, last_key) << "trial " << trial;
            EXPECT_GE(o.predicted_key, o.extracted_key) << "trial " << trial;
            last_key = o.extracted_key;
            if (!o.first_choice_was_minimum()) {
                ++wrong;
            }
        }
        // Every event's features are exactly what the scorer computes for those nodes.
        for (const PredictionEvent& e : observer.events) {
            FeatureVector expected;
            scorer.features_of(e.predicted_node, expected);
            EXPECT_EQ(e.features, expected);
            if (e.has_accepted_example) {
                scorer.features_of(e.accepted_node, expected);
                EXPECT_EQ(e.accepted_features, expected);
                EXPECT_FALSE(e.was_correct);
            }
        }
    }
    EXPECT_GT(predictions, 100U) << "the test must exercise real predictions";
    EXPECT_GT(wrong, 0U) << "and some wrong ones, or the second example is never tested";
}

INSTANTIATE_TEST_SUITE_P(BothStrategies, QueueIdentity,
                         ::testing::Values(ExtractionStrategy::kModular, ExtractionStrategy::kDirect));

TEST(QueueIdentityRetreat, ARetreatReportsNoPredictionAndProducesNoEvent) {
    const CsrGraph<Weight> g = lpq_test::grid_graph(10, 10, Weight{3});
    const auto             meta = compute_metadata(g);
    const GraphFeatures    features(g, meta);
    LogisticPredictor      model;
    OnlineNodeScorer<LogisticPredictor> scorer(features, model);

    AdaptiveController controller{ControllerConfig{}};
    OnlineLearner      learner{OnlineLearningConfig{}};
    const ScorerFeatures feature_source(scorer);
    ControllerObserver observer(controller, &learner, &feature_source);

    // Force the retreat position: an observer whose policy refuses the learned path.
    class Retreat final : public ExtractionObserver {
    public:
        explicit Retreat(ControllerObserver& inner) : inner_(inner) {}
        [[nodiscard]] ExtractionPolicy policy() const noexcept override {
            ExtractionPolicy p;
            p.use_learned_path = false;
            return p;
        }
        void on_extraction(std::uint64_t s, std::uint32_t c, const CorrectionOutcome& o) noexcept override {
            inner_.on_extraction(s, c, o);
        }
    private:
        ControllerObserver& inner_;
    } retreat(observer);

    using Q = LearnedPriorityQueue<Weight, OnlineNodeScorer<LogisticPredictor>>;
    Q queue(4, window(2, 8));
    queue.set_scorer(&scorer);
    queue.set_observer(&retreat);
    (void)dijkstra<Weight>(g, 0, queue);

    EXPECT_GT(queue.metrics().extractions, 0U);
    EXPECT_EQ(learner.metrics().events_recorded, 0U) << "nothing was predicted, so nothing was learned";
    EXPECT_EQ(observer.extractions_without_prediction(), queue.metrics().extractions);
}

TEST(LegacyObserver, WithoutAFeatureSourceTheOriginalBehaviourIsKept) {
    // lpq_adaptive has always recorded one event per extraction with empty features. That is
    // preserved so its historical results mean what they meant; it is not the version-2 contract.
    const CsrGraph<Weight> g = lpq_test::grid_graph(8, 8, Weight{5});
    AdaptiveController controller{ControllerConfig{}};
    OnlineLearner      learner{OnlineLearningConfig{}};
    ControllerObserver observer(controller, &learner);

    ConstantNodeScorer scorer;
    LearnedPriorityQueue<Weight, ConstantNodeScorer> queue(4, window(2, 8));
    queue.set_scorer(&scorer);
    queue.set_observer(&observer);
    (void)dijkstra<Weight>(g, 0, queue);

    EXPECT_EQ(learner.metrics().events_recorded, controller.metrics().observations)
        << "one event per observed extraction, as before";
    EXPECT_EQ(learner.metrics().examples_recorded, learner.metrics().events_recorded);
}

}  // namespace
}  // namespace lpq
