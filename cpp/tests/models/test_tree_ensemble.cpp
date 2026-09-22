// =============================================================================
// Tree ensemble inference (ESD.md section 12, ESD.md section 13).
//
// This model is fitted offline in Python and only evaluated here, so most of
// these tests concern the boundary the file crosses: a bad node index would be
// an out-of-bounds read inside the hottest loop in the program, which under a
// release build is silent corruption rather than a crash.
// =============================================================================
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/models/predictor.hpp"
#include "lpq/models/tree_ensemble.hpp"

namespace lpq {
namespace {

TreeNode leaf(float value) {
    TreeNode n;
    n.feature   = TreeNode::kLeafFeature;
    n.threshold = value;
    return n;
}

TreeNode split(Feature f, float threshold, std::uint32_t left, std::uint32_t right) {
    TreeNode n;
    n.feature   = static_cast<std::uint32_t>(f);
    n.threshold = threshold;
    n.left      = left;
    n.right     = right;
    return n;
}

/// A stump: one split, two leaves.
std::vector<TreeNode> stump(Feature f, float threshold, float low, float high) {
    return {split(f, threshold, 1, 2), leaf(low), leaf(high)};
}

// -----------------------------------------------------------------------------
// Evaluation
// -----------------------------------------------------------------------------

TEST(TreeEnsemble, AnEmptyEnsemblePredictsTheBaseScore) {
    TreeEnsemblePredictor m;
    m.set_base_score(0.75F);
    EXPECT_EQ(m.tree_count(), 0U);
    EXPECT_NEAR(m.raw_score(FeatureVector{}), 0.75F, 1e-6F);
}

TEST(TreeEnsemble, EvaluatesASingleStumpOnBothSidesOfTheSplit) {
    TreeEnsemblePredictor m;
    m.add_tree(stump(Feature::kOutDegree, 0.5F, -1.0F, 1.0F));

    FeatureVector low{};
    set_feature(low, Feature::kOutDegree, 0.2F);
    FeatureVector high{};
    set_feature(high, Feature::kOutDegree, 0.9F);

    EXPECT_NEAR(m.raw_score(low), -1.0F, 1e-6F);
    EXPECT_NEAR(m.raw_score(high), 1.0F, 1e-6F);
}

TEST(TreeEnsemble, TakesTheLeftBranchOnEquality) {
    // The convention is x <= threshold goes left. Which side equality falls on
    // is arbitrary, but it must match the Python exporter exactly or the two
    // implementations will disagree on the boundary -- which is precisely where
    // a tree's training data is densest.
    TreeEnsemblePredictor m;
    m.add_tree(stump(Feature::kOutDegree, 0.5F, -1.0F, 1.0F));

    FeatureVector exact{};
    set_feature(exact, Feature::kOutDegree, 0.5F);
    EXPECT_NEAR(m.raw_score(exact), -1.0F, 1e-6F);
}

TEST(TreeEnsemble, SumsAcrossTreesAndAppliesShrinkage) {
    TreeEnsemblePredictor m;
    m.add_tree(stump(Feature::kOutDegree, 0.5F, 1.0F, 2.0F));
    m.add_tree(stump(Feature::kEffectiveKey, 0.5F, 10.0F, 20.0F));
    m.set_base_score(100.0F);
    m.set_learning_rate_shrinkage(0.5F);

    FeatureVector x{};
    set_feature(x, Feature::kOutDegree, 0.9F);    // -> 2.0
    set_feature(x, Feature::kEffectiveKey, 0.1F);  // -> 10.0

    EXPECT_EQ(m.tree_count(), 2U);
    EXPECT_NEAR(m.raw_score(x), ((2.0F + 10.0F) * 0.5F) + 100.0F, 1e-5F);
}

TEST(TreeEnsemble, WalksADeeperTree) {
    // 0: kOutDegree <= 0.5 ? 1 : 2
    // 1: kEffectiveKey <= 0.25 ? 3 : 4
    // 2: leaf 30
    // 3: leaf 10   4: leaf 20
    std::vector<TreeNode> nodes = {split(Feature::kOutDegree, 0.5F, 1, 2),
                                   split(Feature::kEffectiveKey, 0.25F, 3, 4), leaf(30.0F),
                                   leaf(10.0F), leaf(20.0F)};
    TreeEnsemblePredictor m;
    m.add_tree(nodes);

    FeatureVector a{};
    set_feature(a, Feature::kOutDegree, 0.1F);
    set_feature(a, Feature::kEffectiveKey, 0.1F);
    EXPECT_NEAR(m.raw_score(a), 10.0F, 1e-6F);

    FeatureVector b{};
    set_feature(b, Feature::kOutDegree, 0.1F);
    set_feature(b, Feature::kEffectiveKey, 0.9F);
    EXPECT_NEAR(m.raw_score(b), 20.0F, 1e-6F);

    FeatureVector c{};
    set_feature(c, Feature::kOutDegree, 0.9F);
    EXPECT_NEAR(m.raw_score(c), 30.0F, 1e-6F);
}

TEST(TreeEnsemble, ProducesACalibratedConfidenceFromTheRawScore) {
    TreeEnsemblePredictor m;
    m.add_tree(stump(Feature::kOutDegree, 0.5F, -4.0F, 4.0F));

    FeatureVector high{};
    set_feature(high, Feature::kOutDegree, 0.9F);
    const Prediction p = m.predict(high);

    EXPECT_NEAR(p.score, 4.0F, 1e-5F);
    EXPECT_NEAR(p.confidence, logistic(4.0F), 1e-5F);
    EXPECT_LT(p.uncertainty, 0.1F);
}

TEST(TreeEnsemble, IsDeterministic) {
    TreeEnsemblePredictor m;
    Rng                   rng(31U);
    for (int t = 0; t < 20; ++t) {
        m.add_tree(stump(Feature::kQueueSize, static_cast<float>(rng.next_double(0.0, 1.0)),
                         static_cast<float>(rng.next_double(-1.0, 1.0)),
                         static_cast<float>(rng.next_double(-1.0, 1.0))));
    }

    FeatureVector x{};
    set_feature(x, Feature::kQueueSize, 0.42F);
    const float first = m.raw_score(x);
    for (int i = 0; i < 200; ++i) {
        EXPECT_EQ(m.raw_score(x), first);
    }
}

// -----------------------------------------------------------------------------
// Rejection of malformed trees
// -----------------------------------------------------------------------------

TEST(TreeEnsemble, RejectsAnEmptyTree) {
    TreeEnsemblePredictor m;
    EXPECT_THROW(m.add_tree({}), InvalidConfigError);
}

TEST(TreeEnsemble, RejectsASplitOnAFeatureOutsideTheLayout) {
    TreeEnsemblePredictor m;
    TreeNode              bad;
    bad.feature   = static_cast<std::uint32_t>(kFeatureCount);  // one past the end
    bad.threshold = 0.0F;
    bad.left      = 1;
    bad.right     = 2;
    EXPECT_THROW(m.add_tree({bad, leaf(0.0F), leaf(1.0F)}), InvalidConfigError);
}

TEST(TreeEnsemble, RejectsAChildIndexOutsideTheTree) {
    TreeEnsemblePredictor m;
    EXPECT_THROW(m.add_tree({split(Feature::kOutDegree, 0.0F, 1, 99), leaf(0.0F), leaf(1.0F)}),
                 InvalidConfigError);
}

TEST(TreeEnsemble, RejectsAnEdgeThatWouldMakeTheWalkLoopForever) {
    TreeEnsemblePredictor m;
    // A node pointing at itself, and one pointing backwards.
    EXPECT_THROW(m.add_tree({split(Feature::kOutDegree, 0.0F, 0, 2), leaf(0.0F), leaf(1.0F)}),
                 InvalidConfigError);
    EXPECT_THROW(m.add_tree({split(Feature::kOutDegree, 0.0F, 1, 2), leaf(0.0F),
                             split(Feature::kOutDegree, 0.0F, 1, 1)}),
                 InvalidConfigError);
}

TEST(TreeEnsemble, RejectsNonFiniteThresholdsAndLeaves) {
    TreeEnsemblePredictor m;
    const float           nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(m.add_tree({split(Feature::kOutDegree, nan, 1, 2), leaf(0.0F), leaf(1.0F)}),
                 InvalidConfigError);
    EXPECT_THROW(m.add_tree({split(Feature::kOutDegree, 0.0F, 1, 2), leaf(nan), leaf(1.0F)}),
                 InvalidConfigError);
    EXPECT_THROW(m.set_base_score(nan), InvalidConfigError);
}

// -----------------------------------------------------------------------------
// Serialization
// -----------------------------------------------------------------------------

TEST(TreeEnsemble, RoundTripsExactly) {
    TreeEnsemblePredictor original;
    original.add_tree(stump(Feature::kOutDegree, 0.5F, -1.0F, 1.0F));
    original.add_tree({split(Feature::kEffectiveKey, 0.25F, 1, 2), leaf(3.0F),
                       split(Feature::kQueueSize, 0.75F, 3, 4), leaf(5.0F), leaf(7.0F)});
    original.set_base_score(-0.5F);
    original.set_learning_rate_shrinkage(0.3F);
    original.set_calibrator(TemperatureScaler(1.5F));

    const TreeEnsemblePredictor loaded = round_trip(original);

    EXPECT_EQ(loaded.tree_count(), original.tree_count());
    EXPECT_EQ(loaded.node_count(), original.node_count());

    Rng rng(202U);
    for (int trial = 0; trial < 500; ++trial) {
        FeatureVector x{};
        for (std::size_t i = 0; i < kFeatureCount; ++i) {
            x[i] = static_cast<float>(rng.next_double(-1.0, 1.0));
        }
        // Exact equality: a model whose predictions shifted after a save/load
        // would produce a different search path from the one that was
        // evaluated (ESD.md section 13 step 6).
        EXPECT_EQ(loaded.raw_score(x), original.raw_score(x)) << "trial " << trial;
        EXPECT_EQ(loaded.predict(x).confidence, original.predict(x).confidence);
    }
}

TEST(TreeEnsemble, RefusesAFileWithACorruptChildIndex) {
    // Build a valid file, then corrupt one child index in place. Validation on
    // load is the only thing standing between this and an out-of-bounds read
    // in the evaluation loop.
    std::stringstream s(std::ios::in | std::ios::out | std::ios::binary);
    write_model_header(s, ModelKind::kTreeEnsemble);
    detail::put_f32(s, 0.0F);  // base score
    detail::put_f32(s, 1.0F);  // shrinkage
    detail::put_f32(s, 1.0F);  // temperature
    detail::put_u64(s, 1U);    // one tree
    detail::put_u32(s, 0U);    // offset
    detail::put_u64(s, 3U);    // three nodes
    detail::put_u32(s, static_cast<std::uint32_t>(Feature::kOutDegree));
    detail::put_f32(s, 0.5F);
    detail::put_u32(s, 1U);
    detail::put_u32(s, 77U);  // corrupt: outside the tree
    detail::put_u32(s, TreeNode::kLeafFeature);
    detail::put_f32(s, 0.0F);
    detail::put_u32(s, 0U);
    detail::put_u32(s, 0U);
    detail::put_u32(s, TreeNode::kLeafFeature);
    detail::put_f32(s, 1.0F);
    detail::put_u32(s, 0U);
    detail::put_u32(s, 0U);
    s.seekg(0);

    EXPECT_THROW((void)TreeEnsemblePredictor::load(s), ModelSerializationError);
}

TEST(TreeEnsemble, RefusesAFileDeclaringAnImplausibleTreeCount) {
    std::stringstream s(std::ios::in | std::ios::out | std::ios::binary);
    write_model_header(s, ModelKind::kTreeEnsemble);
    detail::put_f32(s, 0.0F);
    detail::put_f32(s, 1.0F);
    detail::put_f32(s, 1.0F);
    detail::put_u64(s, 1ULL << 40);
    s.seekg(0);

    EXPECT_THROW((void)TreeEnsemblePredictor::load(s), ModelSerializationError);
}

// -----------------------------------------------------------------------------
// Online learning
// -----------------------------------------------------------------------------

TEST(TreeEnsemble, ReportsThatItCannotLearnOnlineRatherThanSilentlyIgnoringUpdates) {
    // A model that silently discarded updates would show a flat learning curve
    // in the online-learning experiments, which looks like a result rather than
    // an exclusion.
    TreeEnsemblePredictor m;
    m.add_tree(stump(Feature::kOutDegree, 0.5F, -1.0F, 1.0F));
    EXPECT_FALSE(m.supports_online_update());

    FeatureVector x{};
    set_feature(x, Feature::kOutDegree, 0.9F);
    const float before = m.raw_score(x);

    TrainingExample ex;
    ex.features = x;
    ex.target   = 0.0F;
    for (int i = 0; i < 100; ++i) {
        m.update(ex);
    }
    EXPECT_EQ(m.raw_score(x), before);
}

}  // namespace
}  // namespace lpq
