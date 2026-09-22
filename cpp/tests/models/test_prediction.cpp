// =============================================================================
// Prediction types and the predictor interface (ESD.md sections 10 and 12).
// =============================================================================
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <cmath>
#include <sstream>
#include <utility>

#include "lpq/common/rng.hpp"
#include <vector>

#include "lpq/models/heuristic_predictor.hpp"
#include "lpq/models/linear_predictor.hpp"
#include "lpq/models/logistic_predictor.hpp"
#include "lpq/models/mlp_predictor.hpp"
#include "lpq/models/predictor.hpp"
#include "lpq/models/tree_ensemble.hpp"

namespace lpq {
namespace {

// -----------------------------------------------------------------------------
// Every model must satisfy the hot-path concept.
//
// These are compile-time assertions rather than runtime tests: a model that
// fails one cannot be used by the templated search at all, so catching it at
// build time is both possible and better.
// -----------------------------------------------------------------------------
static_assert(PredictorLike<ConstantPredictor>);
static_assert(PredictorLike<KeyHeuristicPredictor>);
static_assert(PredictorLike<LinearPredictor>);
static_assert(PredictorLike<LogisticPredictor>);
static_assert(PredictorLike<MlpPredictor>);
static_assert(PredictorLike<TreeEnsemblePredictor>);

// The concept must actually discriminate. A concept every type satisfies would
// pass the assertions above while providing no guarantee at all.
struct NotAPredictor {
    int x = 0;
};
static_assert(!PredictorLike<NotAPredictor>);

// A type with the right members but a mutating predict() must be rejected:
// inference that mutates the model makes predictions depend on evaluation
// order, which would break reproducibility from a seed.
struct MutatingPredict {
    // [[maybe_unused]]: the concept rejects this type on predict()'s constness
    // before it ever looks at these, so nothing reads them.
    [[maybe_unused]] static constexpr ModelKind        kKind = ModelKind::kLinear;
    [[maybe_unused]] static constexpr std::string_view kName = "mutating";
    int                                                calls = 0;

    Prediction predict(const FeatureVector&) {  // deliberately non-const
        ++calls;
        return {};
    }
    void update(const TrainingExample&) {}
    [[nodiscard]] bool supports_online_update() const noexcept { return false; }
    void               save(std::ostream&) const {}
};
static_assert(!PredictorLike<MutatingPredict>);

// -----------------------------------------------------------------------------
// Prediction defaults
// -----------------------------------------------------------------------------

TEST(Prediction, DefaultsToNoRankAndMaximalUncertainty) {
    const Prediction p;
    EXPECT_FALSE(p.has_rank());
    EXPECT_EQ(p.estimated_rank, kUnknownRank);
    // Defaulting to certainty would let a consumer act on confidence that was
    // never produced.
    EXPECT_EQ(p.uncertainty, 1.0F);
    EXPECT_EQ(p.confidence, 0.0F);
}

// -----------------------------------------------------------------------------
// Ordering
// -----------------------------------------------------------------------------

TEST(PredictionBetter, OrdersHigherScoresFirst) {
    Prediction a;
    a.node  = 1;
    a.score = 0.2F;
    Prediction b;
    b.node  = 2;
    b.score = 0.9F;

    const PredictionBetter better;
    EXPECT_TRUE(better(b, a));
    EXPECT_FALSE(better(a, b));
}

TEST(PredictionBetter, BreaksTiesOnNodeIdSoOrderingIsReproducible) {
    // An unstable tie-break would make the search path depend on the order
    // candidates happened to be enumerated in, and a run would stop being
    // reproducible from its seed while still producing correct answers -- the
    // hardest kind of irreproducibility to notice.
    Prediction a;
    a.node  = 7;
    a.score = 0.5F;
    Prediction b;
    b.node  = 3;
    b.score = 0.5F;

    const PredictionBetter better;
    EXPECT_TRUE(better(b, a));
    EXPECT_FALSE(better(a, b));
}

TEST(PredictionBetter, SortsATiedSetIntoTheSameOrderRegardlessOfInputOrder) {
    std::vector<Prediction> forward;
    std::vector<Prediction> backward;
    for (NodeId u = 0; u < 8; ++u) {
        Prediction p;
        p.node  = u;
        p.score = 1.0F;  // every score identical
        forward.push_back(p);
    }
    backward.assign(forward.rbegin(), forward.rend());

    std::sort(forward.begin(), forward.end(), PredictionBetter{});
    std::sort(backward.begin(), backward.end(), PredictionBetter{});

    for (std::size_t i = 0; i < forward.size(); ++i) {
        EXPECT_EQ(forward[i].node, backward[i].node) << "position " << i;
    }
}

// -----------------------------------------------------------------------------
// The runtime-polymorphic adapter
// -----------------------------------------------------------------------------

TEST(PredictorAdapter, ForwardsIdentityAndInference) {
    LinearPredictor model;
    model.set_weight(Feature::kEffectiveKey, -2.0F);

    const std::unique_ptr<Predictor> handle = make_predictor(model);

    EXPECT_EQ(handle->kind(), ModelKind::kLinear);
    EXPECT_EQ(handle->name(), "linear");
    EXPECT_TRUE(handle->supports_online_update());

    FeatureVector x{};
    set_feature(x, Feature::kEffectiveKey, 0.5F);
    EXPECT_NEAR(handle->predict(x).score, model.predict(x).score, 1e-6F);
}

TEST(PredictorAdapter, StampsTheNodeId) {
    const std::unique_ptr<Predictor> handle = make_predictor(ConstantPredictor(1.0F));
    const FeatureVector              x{};
    EXPECT_EQ(handle->predict_for(42U, x).node, 42U);
    // predict() itself leaves the node alone; only predict_for() sets it.
    EXPECT_EQ(handle->predict(x).node, 0U);
}

TEST(PredictorAdapter, ReportsWhetherAModelActuallyLearns) {
    // Reported rather than inferred: a no-op update is indistinguishable at
    // runtime from a real one that happened to have no effect, and the online
    // learning experiments must be able to exclude models that cannot learn
    // instead of recording a flat curve that looks like a result.
    EXPECT_FALSE(make_predictor(ConstantPredictor{})->supports_online_update());
    EXPECT_FALSE(make_predictor(KeyHeuristicPredictor{})->supports_online_update());
    EXPECT_FALSE(make_predictor(TreeEnsemblePredictor{})->supports_online_update());
    EXPECT_TRUE(make_predictor(LinearPredictor{})->supports_online_update());
    EXPECT_TRUE(make_predictor(LogisticPredictor{})->supports_online_update());
    EXPECT_TRUE(make_predictor(MlpPredictor{})->supports_online_update());
}

TEST(PredictorAdapter, HoldsAHeterogeneousModelList) {
    std::vector<std::unique_ptr<Predictor>> models;
    models.push_back(make_predictor(ConstantPredictor(0.0F)));
    models.push_back(make_predictor(KeyHeuristicPredictor{}));
    models.push_back(make_predictor(LinearPredictor{}));
    models.push_back(make_predictor(LogisticPredictor{}));
    models.push_back(make_predictor(MlpPredictor(8)));

    FeatureVector x{};
    set_feature(x, Feature::kEffectiveKey, 0.25F);

    for (const auto& m : models) {
        const Prediction p = m->predict(x);
        EXPECT_TRUE(std::isfinite(p.score)) << m->name();
        EXPECT_GE(p.confidence, 0.0F) << m->name();
        EXPECT_LE(p.confidence, 1.0F) << m->name();
        EXPECT_GE(p.uncertainty, 0.0F) << m->name();
        EXPECT_LE(p.uncertainty, 1.0F) << m->name();
    }
}

TEST(PredictorAdapter, UpdateReachesTheWrappedModel) {
    auto handle = std::make_unique<PredictorAdapter<LogisticPredictor>>(LogisticPredictor{});
    EXPECT_EQ(handle->model().updates_applied(), 0U);

    TrainingExample ex;
    set_feature(ex.features, Feature::kEffectiveKey, 1.0F);
    ex.target = 1.0F;
    handle->update(ex);

    EXPECT_EQ(handle->model().updates_applied(), 1U);
}

// -----------------------------------------------------------------------------
// Baselines
// -----------------------------------------------------------------------------

TEST(ConstantPredictor, IgnoresFeaturesEntirely) {
    const ConstantPredictor m(3.0F, 0.5F);

    FeatureVector a{};
    FeatureVector b{};
    set_feature(b, Feature::kEffectiveKey, 99.0F);
    set_feature(b, Feature::kOutDegree, 12.0F);

    EXPECT_EQ(m.predict(a).score, m.predict(b).score);
    // Maximal uncertainty always: this model has no basis for preferring one
    // candidate over another.
    EXPECT_EQ(m.predict(b).uncertainty, 1.0F);
}

TEST(ConstantPredictor, InducesNoOrdering) {
    const ConstantPredictor m;
    const PredictionBetter  better;

    FeatureVector low{};
    set_feature(low, Feature::kEffectiveKey, 0.1F);
    FeatureVector high{};
    set_feature(high, Feature::kEffectiveKey, 0.9F);

    Prediction a = m.predict(low);
    a.node       = 1;
    Prediction b = m.predict(high);
    b.node       = 2;

    // Neither is preferred on score, so only the node-id tie-break separates
    // them. That is the point: the null model supplies no ordering information.
    EXPECT_EQ(a.score, b.score);
    EXPECT_TRUE(better(a, b));
}

TEST(KeyHeuristicPredictor, RanksTheSmallerKeyFirst) {
    const KeyHeuristicPredictor m;

    FeatureVector near_min{};
    set_feature(near_min, Feature::kEffectiveKey, 0.10F);
    FeatureVector far_off{};
    set_feature(far_off, Feature::kEffectiveKey, 0.80F);

    EXPECT_GT(m.predict(near_min).score, m.predict(far_off).score);
}

// The baseline is at the accuracy ceiling by construction: for exact Dijkstra
// the correct next extraction IS the smallest key, and the key is a feature.
// This test states that fact as an executable claim, because the research
// question depends on it -- a learned model cannot justify itself by beating
// this on accuracy, only by beating the classical queues on end-to-end time.
TEST(KeyHeuristicPredictor, PerfectlyRanksAnyCandidateSetByKey) {
    const KeyHeuristicPredictor m;
    Rng                         rng(4242U);

    for (int trial = 0; trial < 200; ++trial) {
        std::vector<std::pair<float, Prediction>> candidates;
        for (NodeId u = 0; u < 16; ++u) {
            const auto    key = static_cast<float>(rng.next_double(0.0, 1.0));
            FeatureVector x{};
            set_feature(x, Feature::kEffectiveKey, key);
            Prediction p = m.predict(x);
            p.node       = u;
            candidates.emplace_back(key, p);
        }

        const auto best = std::min_element(
            candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        const auto predicted = std::max_element(
            candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b) { return a.second.score < b.second.score; });

        EXPECT_EQ(best->second.node, predicted->second.node) << "trial " << trial;
    }
}

TEST(KeyHeuristicPredictor, IsMostConfidentAtTheQueueMinimum) {
    const KeyHeuristicPredictor m;

    FeatureVector at_min{};
    set_feature(at_min, Feature::kKeyGapFromMinimum, 0.0F);
    FeatureVector above{};
    set_feature(above, Feature::kKeyGapFromMinimum, 2.0F);

    EXPECT_NEAR(m.predict(at_min).confidence, 1.0F, 1e-6F);
    EXPECT_LT(m.predict(above).confidence, m.predict(at_min).confidence);
    EXPECT_GT(m.predict(above).confidence, 0.0F);
}

TEST(Baselines, DoNotChangeUnderUpdate) {
    KeyHeuristicPredictor m;
    FeatureVector         x{};
    set_feature(x, Feature::kEffectiveKey, 0.3F);
    const float before = m.predict(x).score;

    TrainingExample ex;
    ex.features = x;
    ex.target   = 1.0F;
    for (int i = 0; i < 100; ++i) {
        m.update(ex);
    }
    EXPECT_EQ(m.predict(x).score, before);
}

}  // namespace
}  // namespace lpq
