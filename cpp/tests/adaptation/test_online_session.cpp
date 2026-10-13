// =============================================================================
// The online scorer and session, and frozen equivalence with lpq_model (M5-O3, O4).
//
// "Frozen equivalence" is the claim that makes online learning safe to wire in: with updates
// disabled -- and, because updates are applied BETWEEN queries, with updates enabled too, for the
// query being searched -- an lpq_online search is the lpq_model search. Same artifact, same
// feature vectors, same scoring, same tie-breaking, same queue configuration => same distances,
// same parents, same counters, same learned-queue metrics. These are differential tests over random
// graphs with explicit seeds, plus the independent check that every answer equals the trusted
// reference. A seed that ever fails becomes a named regression test; none has.
// =============================================================================
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "lpq/adaptation/online_session.hpp"
#include "lpq/algorithms/dijkstra.hpp"
#include "lpq/algorithms/reference_dijkstra.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/graph/graph_metadata.hpp"
#include "lpq/lpq/learned_priority_queue.hpp"
#include "lpq_test/graph_fixtures.hpp"

namespace lpq {
namespace {

using Weight       = std::uint32_t;
using StaticScorer = StaticNodeScorer<LogisticPredictor>;
using StaticQueue  = LearnedPriorityQueue<Weight, StaticScorer>;
using OnlineQueue  = LearnedPriorityQueue<Weight, OnlineSession::Scorer>;

CandidateWindowConfig window(std::uint32_t lo, std::uint32_t hi) {
    CandidateWindowConfig c;
    c.k_min = lo;
    c.k_max = hi;
    return c;
}

LogisticPredictor random_model(Rng& rng) {
    LogisticPredictor m;
    const Feature statics[] = {Feature::kOutDegree,        Feature::kMinAdjacentWeight,
                               Feature::kMeanAdjacentWeight, Feature::kMaxAdjacentWeight,
                               Feature::kAdjacentWeightSpread, Feature::kDegreeRelativeToMean};
    for (const Feature f : statics) {
        m.set_weight(f, static_cast<float>(rng.next_unit() * 6.0 - 3.0));
    }
    m.set_bias(static_cast<float>(rng.next_unit() * 2.0 - 1.0));
    return m;
}

struct Case {
    CsrGraph<Weight> graph;
    NodeId           source;
};

Case random_case(Rng& rng, int trial) {
    lpq_test::RandomGraphSpec spec;
    spec.min_nodes        = 6;
    spec.max_nodes        = 80;
    spec.edge_probability = 0.2;
    spec.max_weight       = 50;
    spec.many_ties        = (trial % 4 == 0);
    spec.directedness     = (trial % 2 == 0) ? Directedness::kDirected : Directedness::kUndirected;
    CsrGraph<Weight> g    = lpq_test::random_graph(rng, spec);
    const NodeId     s    = rng.next_node(static_cast<NodeId>(g.num_nodes()));
    return {std::move(g), s};
}

void expect_same_run(const ShortestPathResult<Weight>& a, const ShortestPathResult<Weight>& b,
                     const std::string& what) {
    EXPECT_EQ(a.dist, b.dist) << what;
    EXPECT_EQ(a.parent, b.parent) << what;
    EXPECT_EQ(a.counters.pq_pushes, b.counters.pq_pushes) << what;
    EXPECT_EQ(a.counters.pq_pops, b.counters.pq_pops) << what;
    EXPECT_EQ(a.counters.pq_comparisons, b.counters.pq_comparisons) << what;
    EXPECT_EQ(a.counters.stale_pops, b.counters.stale_pops) << what;
    EXPECT_EQ(a.counters.settled_nodes, b.counters.settled_nodes) << what;
    EXPECT_EQ(a.counters.relaxations, b.counters.relaxations) << what;
    EXPECT_EQ(a.counters.successful_relaxations, b.counters.successful_relaxations) << what;
}

template <typename QA, typename QB>
void expect_same_queue_metrics(const QA& a, const QB& b, const std::string& what) {
    EXPECT_EQ(a.metrics().extractions, b.metrics().extractions) << what;
    EXPECT_EQ(a.metrics().verified_extractions, b.metrics().verified_extractions) << what;
    EXPECT_EQ(a.metrics().fallback_scans, b.metrics().fallback_scans) << what;
    EXPECT_EQ(a.metrics().entries_examined, b.metrics().entries_examined) << what;
    EXPECT_EQ(a.metrics().entries_skipped, b.metrics().entries_skipped) << what;
    EXPECT_EQ(a.metrics().window_divergences, b.metrics().window_divergences) << what;
    EXPECT_EQ(a.candidate_metrics().candidates_offered, b.candidate_metrics().candidates_offered) << what;
    EXPECT_EQ(a.candidate_metrics().candidates_admitted, b.candidate_metrics().candidates_admitted) << what;
    EXPECT_EQ(a.candidate_metrics().expansions, b.candidate_metrics().expansions) << what;
    EXPECT_EQ(a.verification_metrics().comparisons, b.verification_metrics().comparisons) << what;
    EXPECT_EQ(a.correction_metrics().first_attempt, b.correction_metrics().first_attempt) << what;
    EXPECT_EQ(a.correction_metrics().fallbacks, b.correction_metrics().fallbacks) << what;
}

OnlineLearningConfig learner_config(UpdateMode mode) {
    OnlineLearningConfig c;
    c.mode            = mode;
    c.interval        = 16;
    c.buffer_capacity = 256;
    c.max_batch       = 32;
    return c;
}

// -----------------------------------------------------------------------------
// The scorer
// -----------------------------------------------------------------------------

TEST(OnlineScorer, AgreesBitForBitWithTheStaticScorerOnEveryNode) {
    Rng rng(20261014U);
    for (int trial = 0; trial < 40; ++trial) {
        const Case         c = random_case(rng, trial);
        const auto         meta = compute_metadata(c.graph);
        const GraphFeatures features(c.graph, meta);
        const LogisticPredictor model = random_model(rng);

        StaticScorer frozen;
        frozen.build(features, model);
        const OnlineSession::Scorer online(features, model);

        for (NodeId u = 0; u < c.graph.num_nodes(); ++u) {
            const float a = frozen.score_of(u);
            const float b = online.score_of(u);
            ASSERT_EQ(std::memcmp(&a, &b, sizeof a), 0) << "trial " << trial << " node " << u;
        }
        // Out-of-range behaves the same too.
        EXPECT_EQ(frozen.score_of(static_cast<NodeId>(c.graph.num_nodes() + 5)),
                  online.score_of(static_cast<NodeId>(c.graph.num_nodes() + 5)));
    }
}

TEST(OnlineScorer, TrainsOnExactlyTheVectorItScoresWith) {
    const CsrGraph<Weight> g = lpq_test::grid_graph(6, 6, Weight{4});
    const auto             meta = compute_metadata(g);
    const GraphFeatures    features(g, meta);
    LogisticPredictor      model;
    OnlineSession          session(features, model, learner_config(UpdateMode::kDisabled));
    for (NodeId u = 0; u < g.num_nodes(); ++u) {
        FeatureVector from_scorer;
        FeatureVector from_source;
        FeatureVector direct;
        session.scorer().features_of(u, from_scorer);
        session.features_of(u, from_source);
        fill_static_features(features.node(u), direct);
        EXPECT_EQ(from_scorer, direct);
        EXPECT_EQ(from_source, direct);
    }
}

TEST(OnlineScorer, ReadsTheLiveParametersWithNoCache) {
    // The reason for computing on demand: there is no stored score that an update could leave stale.
    const CsrGraph<Weight> g = lpq_test::grid_graph(5, 5, Weight{3});
    const auto             meta = compute_metadata(g);
    const GraphFeatures    features(g, meta);
    LogisticPredictor      model;
    const OnlineSession::Scorer scorer(features, model);
    const float before = scorer.score_of(7);
    model.set_bias(model.bias() + 2.0F);
    EXPECT_EQ(scorer.score_of(7), before + 2.0F);
}

TEST(OnlineScorer, ANonFiniteScoreBecomesZeroAndNeverReachesTheQueue) {
    const CsrGraph<Weight> g = lpq_test::grid_graph(5, 5, Weight{3});
    const auto             meta = compute_metadata(g);
    const GraphFeatures    features(g, meta);
    for (const float poison : {std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(), 1e30F}) {
        LogisticPredictor model;
        model.set_weight(Feature::kOutDegree, poison);
        model.set_weight(Feature::kMeanAdjacentWeight, -poison);
        const OnlineSession::Scorer scorer(features, model);
        for (NodeId u = 0; u < g.num_nodes(); ++u) {
            EXPECT_TRUE(std::isfinite(scorer.score_of(u)));
        }
    }
}

// -----------------------------------------------------------------------------
// Frozen equivalence: a search with an online scorer IS the lpq_model search
// -----------------------------------------------------------------------------

struct Mode {
    ExtractionStrategy strategy;
    bool               approximate;
    const char*        name;
};

class FrozenEquivalence : public ::testing::TestWithParam<Mode> {};

TEST_P(FrozenEquivalence, AnOnlineSearchEqualsTheModelSearchRunForRun) {
    const Mode mode = GetParam();
    Rng        rng(20261015U);
    int        compared = 0;

    for (int trial = 0; trial < 50; ++trial) {
        const Case          c = random_case(rng, trial);
        const auto          meta = compute_metadata(c.graph);
        const GraphFeatures features(c.graph, meta);
        const LogisticPredictor model = random_model(rng);
        const Weight        width = static_cast<Weight>(1 + rng.next_below(15));
        const auto          win = window(2, 2 + static_cast<std::uint32_t>(rng.next_below(12)));
        const CorrectnessPolicy policy =
            mode.approximate ? CorrectnessPolicy::approximate(0.2) : CorrectnessPolicy::exact();

        // The reference answer, independent of both queues.
        const auto reference = reference_dijkstra<Weight>(c.graph, c.source);

        // lpq_model: precomputed scores, no observer.
        StaticScorer frozen;
        frozen.build(features, model);
        StaticQueue static_queue(width, win, policy, mode.strategy);
        static_queue.set_scorer(&frozen);
        const auto expected = dijkstra<Weight>(c.graph, c.source, static_queue);

        // lpq_online, every update mode: updates never happen during a search, so each must match.
        for (const UpdateMode update : {UpdateMode::kDisabled, UpdateMode::kPeriodic,
                                        UpdateMode::kEveryEvent, UpdateMode::kAdaptive}) {
            OnlineSession session(features, model, learner_config(update));
            OnlineQueue   online_queue(width, win, policy, mode.strategy);
            online_queue.set_scorer(&session.scorer());
            online_queue.set_observer(&session.observer());
            const auto actual = dijkstra<Weight>(c.graph, c.source, online_queue);

            const std::string what = std::string(mode.name) + " trial " + std::to_string(trial) +
                                     " " + std::string(update_mode_name(update));
            expect_same_run(expected, actual, what);
            expect_same_queue_metrics(static_queue, online_queue, what);
            EXPECT_EQ(session.current_hash(), session.start_hash()) << what << ": nothing applied mid-search";
            if (!mode.approximate) {
                EXPECT_EQ(actual.dist, reference.dist) << what << ": exact answer vs the reference";
            }
            ++compared;
        }
    }
    EXPECT_EQ(compared, 200);
}

INSTANTIATE_TEST_SUITE_P(
    AllConfigurations, FrozenEquivalence,
    ::testing::Values(Mode{ExtractionStrategy::kModular, false, "modular-exact"},
                      Mode{ExtractionStrategy::kDirect, false, "direct-exact"},
                      Mode{ExtractionStrategy::kModular, true, "modular-approximate"}));

// -----------------------------------------------------------------------------
// Correctness never depends on the model
// -----------------------------------------------------------------------------

TEST(SessionCorrectness, EveryQueryOfALearningSessionStaysExact) {
    Rng           rng(20261016U);
    std::uint64_t sessions_that_changed = 0;
    std::uint64_t total_updates         = 0;

    for (int trial = 0; trial < 30; ++trial) {
        const Case          c = random_case(rng, trial);
        const auto          meta = compute_metadata(c.graph);
        const GraphFeatures features(c.graph, meta);
        OnlineSession       session(features, random_model(rng), learner_config(UpdateMode::kPeriodic));

        for (int q = 0; q < 6; ++q) {
            const NodeId source = rng.next_node(static_cast<NodeId>(c.graph.num_nodes()));
            OnlineQueue  queue(static_cast<Weight>(1 + rng.next_below(10)), window(2, 10),
                               CorrectnessPolicy::exact(),
                               q % 2 == 0 ? ExtractionStrategy::kModular : ExtractionStrategy::kDirect);
            queue.set_scorer(&session.scorer());
            queue.set_observer(&session.observer());
            const auto actual    = dijkstra<Weight>(c.graph, source, queue);
            const auto reference = reference_dijkstra<Weight>(c.graph, source);
            ASSERT_EQ(actual.dist, reference.dist) << "trial " << trial << " query " << q;

            (void)session.end_query();
            ASSERT_TRUE(session.learner().accounting_balanced()) << "trial " << trial;
            ASSERT_TRUE(parameters_within(session.model(), 1000.0F));
        }
        total_updates += session.learner().metrics().updates_performed;
        if (session.weights_changed()) ++sessions_that_changed;
    }
    EXPECT_GT(total_updates, 0U) << "the test must actually apply updates";
    EXPECT_GT(sessions_that_changed, 0U) << "and some must move the weights, or it proves nothing";
}

TEST(SessionCorrectness, PoisonedParametersStillGiveExactAnswers) {
    Rng rng(20261017U);
    for (int trial = 0; trial < 12; ++trial) {
        const Case          c = random_case(rng, trial);
        const auto          meta = compute_metadata(c.graph);
        const GraphFeatures features(c.graph, meta);
        LogisticPredictor   model;
        const float poison[] = {std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity(), -1e30F, 1e30F};
        model.set_weight(Feature::kOutDegree, poison[trial % 4]);
        model.set_weight(Feature::kMeanAdjacentWeight, poison[(trial + 1) % 4]);
        model.set_bias(poison[(trial + 2) % 4]);
        OnlineSession session(features, model, learner_config(UpdateMode::kDisabled));
        OnlineQueue   queue(3, window(2, 8));
        queue.set_scorer(&session.scorer());
        queue.set_observer(&session.observer());
        EXPECT_EQ(dijkstra<Weight>(c.graph, c.source, queue).dist,
                  reference_dijkstra<Weight>(c.graph, c.source).dist)
            << "trial " << trial;
    }
}

TEST(SessionCorrectness, AHaltedLearnerLeavesTheSearchRunningAndTheModelUntouched) {
    Rng rng(20261018U);
    const Case          c = random_case(rng, 3);
    const auto          meta = compute_metadata(c.graph);
    const GraphFeatures features(c.graph, meta);
    auto                lc = learner_config(UpdateMode::kPeriodic);
    lc.max_abs_weight      = 1e-9F;  // every real update fails the range check
    lc.max_rollbacks       = 1;
    OnlineSession session(features, random_model(rng), lc);
    const auto    start = session.start_hash();

    for (int q = 0; q < 5; ++q) {
        const NodeId source = rng.next_node(static_cast<NodeId>(c.graph.num_nodes()));
        OnlineQueue  queue(4, window(2, 8));
        queue.set_scorer(&session.scorer());
        queue.set_observer(&session.observer());
        ASSERT_EQ(dijkstra<Weight>(c.graph, source, queue).dist,
                  reference_dijkstra<Weight>(c.graph, source).dist);
        (void)session.end_query();
        ASSERT_TRUE(session.learner().accounting_balanced());
    }
    EXPECT_TRUE(session.learner().halted());
    EXPECT_EQ(session.current_hash(), start);
    EXPECT_FALSE(session.weights_changed());
}

// -----------------------------------------------------------------------------
// Determinism and honest reporting
// -----------------------------------------------------------------------------

TEST(SessionDeterminism, TheSameQueriesGiveTheSameParameterTrajectory) {
    const auto trajectory = [] {
        Rng           rng(20261019U);
        const Case    c = random_case(rng, 5);
        const auto    meta = compute_metadata(c.graph);
        const GraphFeatures features(c.graph, meta);
        OnlineSession session(features, random_model(rng), learner_config(UpdateMode::kPeriodic));
        std::vector<std::uint64_t> hashes;
        for (int q = 0; q < 8; ++q) {
            const NodeId source = rng.next_node(static_cast<NodeId>(c.graph.num_nodes()));
            OnlineQueue  queue(5, window(2, 9));
            queue.set_scorer(&session.scorer());
            queue.set_observer(&session.observer());
            (void)dijkstra<Weight>(c.graph, source, queue);
            (void)session.end_query();
            hashes.push_back(session.current_hash());
        }
        return hashes;
    };
    EXPECT_EQ(trajectory(), trajectory());
}

TEST(SessionReporting, WeightsChangedIsMeasuredNotInferredFromTheUpdateCount) {
    const CsrGraph<Weight> g = lpq_test::grid_graph(6, 6, Weight{2});
    const auto             meta = compute_metadata(g);
    const GraphFeatures    features(g, meta);
    OnlineSession          disabled(features, LogisticPredictor{}, learner_config(UpdateMode::kDisabled));
    OnlineQueue            q1(3, window(2, 8));
    q1.set_scorer(&disabled.scorer());
    q1.set_observer(&disabled.observer());
    (void)dijkstra<Weight>(g, 0, q1);
    EXPECT_EQ(disabled.end_query(), 0U);
    EXPECT_FALSE(disabled.weights_changed());
    EXPECT_GT(disabled.learner().metrics().events_recorded, 0U) << "events are still recorded";
    EXPECT_EQ(disabled.learner().metrics().updates_performed, 0U);
}

}  // namespace
}  // namespace lpq
