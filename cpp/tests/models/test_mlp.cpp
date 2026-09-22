// =============================================================================
// Small MLP predictor (ESD.md section 12: "Small MLP if experimentally
// justified").
// =============================================================================
#include <gtest/gtest.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <vector>

#include "lpq/common/error.hpp"
#include "lpq/common/rng.hpp"
#include "lpq/models/logistic_predictor.hpp"
#include "lpq/models/mlp_predictor.hpp"

namespace lpq {
namespace {

// -----------------------------------------------------------------------------
// Shape
// -----------------------------------------------------------------------------

TEST(MlpPredictor, RejectsAHiddenWidthOutsideTheSupportedRange) {
    // A larger network is not silently truncated: a model that quietly ran at a
    // different size than it was trained at would mispredict rather than fail.
    EXPECT_THROW(MlpPredictor(0), InvalidConfigError);
    EXPECT_THROW(MlpPredictor(kMaxHiddenUnits + 1), InvalidConfigError);
    // Named, not temporaries: `MlpPredictor(kMaxHiddenUnits)` is parsed as a
    // declaration of a variable called kMaxHiddenUnits, so the assertion would
    // construct nothing and pass unconditionally.
    EXPECT_NO_THROW({ const MlpPredictor smallest(1); });
    EXPECT_NO_THROW({ const MlpPredictor largest(kMaxHiddenUnits); });
}

TEST(MlpPredictor, ReportsItsInferenceCost) {
    // An architecture-independent operation count, used by the model-cost
    // comparison where a timing would not be portable between machines.
    const MlpPredictor m(32);
    EXPECT_EQ(m.inference_flops(), (kFeatureCount * 32U) + 32U);
    // The bar the MLP has to clear: it is well over an order of magnitude more
    // arithmetic than the linear model's one pass over the feature vector.
    EXPECT_GT(m.inference_flops(), kFeatureCount * 10U);
}

TEST(MlpPredictor, StartsAtZeroBeforeInitialisation) {
    const MlpPredictor m(8);
    FeatureVector      x{};
    x.fill(1.0F);
    EXPECT_EQ(m.predict(x).score, 0.0F);
}

// -----------------------------------------------------------------------------
// Initialisation
// -----------------------------------------------------------------------------

TEST(MlpPredictor, InitialisationBreaksSymmetryBetweenHiddenUnits) {
    // Identical hidden units compute the same function and receive the same
    // gradient forever, so a zero-initialised network has the capacity of a
    // single unit no matter how wide it is.
    MlpPredictor m(16);
    Rng          rng(11U);
    m.initialise(rng);

    std::set<float> distinct;
    for (float w : m.output_weights()) {
        distinct.insert(w);
    }
    EXPECT_EQ(distinct.size(), 16U);

    for (float w : m.hidden_weights()) {
        EXPECT_TRUE(std::isfinite(w));
    }
}

TEST(MlpPredictor, InitialisationIsReproducibleFromASeed) {
    MlpPredictor a(12);
    MlpPredictor b(12);
    Rng          ra(20260922U);
    Rng          rb(20260922U);
    a.initialise(ra);
    b.initialise(rb);

    ASSERT_EQ(a.hidden_weights().size(), b.hidden_weights().size());
    for (std::size_t i = 0; i < a.hidden_weights().size(); ++i) {
        EXPECT_EQ(a.hidden_weights()[i], b.hidden_weights()[i]) << "index " << i;
    }
}

TEST(MlpPredictor, DifferentSeedsGiveDifferentInitialisations) {
    MlpPredictor a(12);
    MlpPredictor b(12);
    Rng          ra(1U);
    Rng          rb(2U);
    a.initialise(ra);
    b.initialise(rb);
    EXPECT_NE(a.hidden_weights(), b.hidden_weights());
}

// -----------------------------------------------------------------------------
// Forward pass
// -----------------------------------------------------------------------------

TEST(MlpPredictor, AppliesReluToTheHiddenLayer) {
    // One hidden unit whose pre-activation is driven negative must contribute
    // nothing, and driven positive must contribute linearly.
    MlpPredictor m(1);

    std::vector<float> w1(kFeatureCount, 0.0F);
    w1[static_cast<std::size_t>(Feature::kOutDegree)] = 1.0F;
    m.set_parameters(w1, {0.0F}, {2.0F}, 0.0F);

    FeatureVector negative{};
    set_feature(negative, Feature::kOutDegree, -5.0F);
    EXPECT_EQ(m.predict(negative).score, 0.0F);

    FeatureVector positive{};
    set_feature(positive, Feature::kOutDegree, 3.0F);
    EXPECT_NEAR(m.predict(positive).score, 6.0F, 1e-5F);
}

TEST(MlpPredictor, RejectsParameterArraysThatDoNotMatchItsShape) {
    MlpPredictor m(4);
    EXPECT_THROW(m.set_parameters(std::vector<float>(3 * kFeatureCount, 0.0F),
                                  std::vector<float>(4, 0.0F), std::vector<float>(4, 0.0F), 0.0F),
                 InvalidConfigError);
    EXPECT_THROW(m.set_parameters(std::vector<float>(4 * kFeatureCount, 0.0F),
                                  std::vector<float>(3, 0.0F), std::vector<float>(4, 0.0F), 0.0F),
                 InvalidConfigError);
}

TEST(MlpPredictor, IsDeterministicAcrossRepeatedCalls) {
    MlpPredictor m(24);
    Rng          rng(4711U);
    m.initialise(rng);

    FeatureVector x{};
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        x[i] = static_cast<float>(rng.next_double(-1.0, 1.0));
    }

    const float first = m.predict(x).score;
    for (int i = 0; i < 500; ++i) {
        EXPECT_EQ(m.predict(x).score, first);
    }
}

TEST(MlpPredictor, ConfidenceStaysInRangeForExtremeInputs) {
    MlpPredictor m(16);
    Rng          rng(606U);
    m.initialise(rng);

    for (float magnitude : {1e-6F, 1.0F, 1e6F, 1e20F}) {
        FeatureVector x{};
        x.fill(magnitude);
        const Prediction p = m.predict(x);
        EXPECT_GE(p.confidence, 0.0F) << "magnitude " << magnitude;
        EXPECT_LE(p.confidence, 1.0F) << "magnitude " << magnitude;
    }
}

// -----------------------------------------------------------------------------
// Learning
// -----------------------------------------------------------------------------

struct XorDataset {
    std::vector<FeatureVector> x;
    std::vector<float>         y;
};

/// A problem that is NOT linearly separable: the label is the exclusive-or of
/// the signs of two features.
XorDataset make_xor_dataset(Rng& rng, int n) {
    XorDataset d;
    for (int i = 0; i < n; ++i) {
        FeatureVector v{};
        const auto    a = static_cast<float>(rng.next_double(-1.0, 1.0));
        const auto    b = static_cast<float>(rng.next_double(-1.0, 1.0));
        set_feature(v, Feature::kOutDegree, a);
        set_feature(v, Feature::kEffectiveKey, b);
        d.x.push_back(v);
        d.y.push_back(((a > 0.0F) != (b > 0.0F)) ? 1.0F : 0.0F);
    }
    return d;
}

template <typename Model>
double accuracy(const Model& m, const XorDataset& d) {
    int correct = 0;
    for (std::size_t i = 0; i < d.x.size(); ++i) {
        const bool predicted = m.predict(d.x[i]).score > 0.0F;
        if (predicted == (d.y[i] > 0.5F)) {
            ++correct;
        }
    }
    return static_cast<double>(correct) / static_cast<double>(d.x.size());
}

// The whole reason to carry an MLP at all: it can represent something the
// linear model cannot. This test measures both on the same fixed-seed data, so
// the comparison is a result rather than an assertion.
TEST(MlpPredictor, LearnsAProblemTheLinearModelCannotRepresent) {
    Rng              rng(20260922U);
    const XorDataset train = make_xor_dataset(rng, 60000);
    const XorDataset test  = make_xor_dataset(rng, 10000);

    LinearTrainingConfig config;
    config.learning_rate = 0.05F;
    config.l2            = 0.0F;

    MlpPredictor mlp(16, config);
    Rng          init(7U);
    mlp.initialise(init);

    LogisticPredictor linear(config);

    for (std::size_t i = 0; i < train.x.size(); ++i) {
        TrainingExample ex;
        ex.features = train.x[i];
        ex.target   = train.y[i];
        mlp.update(ex);
        linear.update(ex);
    }

    const double mlp_accuracy    = accuracy(mlp, test);
    const double linear_accuracy = accuracy(linear, test);

    std::cout << "[  MEASURED ] XOR held-out accuracy -- MLP(16): " << mlp_accuracy
              << ", logistic: " << linear_accuracy << std::endl;

    // The linear model is at chance, because no hyperplane separates XOR.
    EXPECT_LT(linear_accuracy, 0.60) << "logistic accuracy on XOR was " << linear_accuracy;
    EXPECT_GT(mlp_accuracy, 0.85) << "MLP accuracy on XOR was " << mlp_accuracy;
    EXPECT_GT(mlp_accuracy, linear_accuracy + 0.20);
}

TEST(MlpPredictor, CountsUpdates) {
    MlpPredictor    m(4);
    TrainingExample ex;
    set_feature(ex.features, Feature::kOutDegree, 1.0F);
    ex.target = 1.0F;

    m.update(ex);
    m.update(ex);
    EXPECT_EQ(m.updates_applied(), 2U);
}

TEST(MlpPredictor, SurvivesAStreamOfHostileExamples) {
    MlpPredictor m(8);
    Rng          rng(9090U);
    m.initialise(rng);

    const std::vector<float> nasty = {std::numeric_limits<float>::quiet_NaN(),
                                      std::numeric_limits<float>::infinity(),
                                      -std::numeric_limits<float>::infinity(),
                                      3.4e38F,
                                      0.0F};

    for (int i = 0; i < 3000; ++i) {
        TrainingExample ex;
        for (std::size_t f = 0; f < kFeatureCount; ++f) {
            ex.features[f] = nasty[static_cast<std::size_t>(rng.next_below(nasty.size()))];
        }
        ex.target = static_cast<float>(rng.next_below(2));
        m.update(ex);
    }

    FeatureVector probe{};
    probe.fill(0.25F);
    EXPECT_TRUE(std::isfinite(m.predict(probe).score));
    for (float w : m.hidden_weights()) {
        EXPECT_TRUE(std::isfinite(w));
    }
    for (float w : m.output_weights()) {
        EXPECT_TRUE(std::isfinite(w));
    }
}

}  // namespace
}  // namespace lpq
